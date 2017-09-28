/*
 * Copyright (c) 2016 Masayoshi Mizutani <mizutani@sfc.wide.ad.jp> All
 * rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <arpa/inet.h>
#include "../module.hpp"
#include "../../external/cpp-toolbox/src/cache.hpp"
#include "../../external/cpp-toolbox/src/buffer.hpp"


namespace pm {

class TCP : public Module {
 private:
  struct tcp_header {
    uint16_t src_port_;  // source port
    uint16_t dst_port_;  // destination port
    uint32_t seq_;       // tcp sequence number
    uint32_t ack_;       // tcp ack number

    // ToDo(Masa): 4 bit data field should be updated for little-endian
    uint8_t offset_;

    uint8_t flags_;      // flags
    uint16_t window_;    // window
    uint16_t chksum_;    // checksum
    uint16_t urgptr_;    // urgent pointer
  } __attribute__((packed));

  static inline std::string flag2str(uint8_t f) {
    std::string s;
    s.append((f & FIN) > 0 ? "F" : "*");
    s.append((f & SYN) > 0 ? "S" : "*");
    s.append((f & RST) > 0 ? "R" : "*");
    s.append((f & ACK) > 0 ? "A" : "*");
    return s;
  }

  const ParamDef* p_src_port_;
  const ParamDef* p_dst_port_;
  const ParamDef* p_seq_;
  const ParamDef* p_ack_;
  const ParamDef* p_offset_;
  const ParamDef* p_flags_;
  const ParamDef* p_window_;
  const ParamDef* p_chksum_;
  const ParamDef* p_urgptr_;

  // Flags
  const ParamDef* p_flag_fin_;
  const ParamDef* p_flag_syn_;
  const ParamDef* p_flag_rst_;
  const ParamDef* p_flag_push_;
  const ParamDef* p_flag_ack_;
  const ParamDef* p_flag_urg_;
  const ParamDef* p_flag_ece_;
  const ParamDef* p_flag_cwr_;

  const ParamDef* p_optdata_;
  const ParamDef* p_segment_;
  const ParamDef* p_ssn_id_;;
  const ParamDef* p_data_;

  const ParamDef* p_rtt_3wh_;
  const ParamDef* p_tx_server_;
  const ParamDef* p_tx_client_;
  const EventDef *ev_new_, *ev_estb_, *ev_close_;

  static const uint8_t FIN  = 0x01;
  static const uint8_t SYN  = 0x02;
  static const uint8_t RST  = 0x04;
  static const uint8_t PUSH = 0x08;
  static const uint8_t ACK  = 0x10;
  static const uint8_t URG  = 0x20;
  static const uint8_t ECE  = 0x40;
  static const uint8_t CWR  = 0x80;

  static const bool DBG         = false;
  static const bool DBG_SEQ     = false;
  static const bool DBG_STAT    = false;
  static const bool DBG_REASS   = false;

  static const time_t TIMEOUT = 300;
  uint64_t ssn_count_;
  time_t curr_ts_;
  bool init_ts_;

  class Session {
   public:
    enum Status {
      NONE,
      SYN_SENT,
      SYNACK_SENT,
      ESTABLISHED,
      CLOSING,
      CLOSED,
    };

    /* State Transition

      -- Client -------------- Server --
      [CLOSING]               [CLOSING]
          |       ---(SYN)--->    |      => SYN_SENT
      [SYN_SENT]              [LISTEN]
          |       <-(SYN|ACK)-    |      => SYNACK_SENT
      [SYN_SENT]              [SYN_RECV]
          |       ---(ACK)-->     |      => ESTABLISHED
      [ESTABLISH]             [SYN_RECV]
          |    <--(ACK or Data)-- |
    */

   private:
    class Segment : public tb::Buffer {
     private:
      uint32_t seq_;
      uint8_t flags_;
      Segment *next_, *tail_;

     public:
      Segment(const void* ptr, size_t len, uint32_t seq, uint8_t flags) :
          tb::Buffer(ptr, len), seq_(seq), flags_(flags),
          next_(nullptr), tail_(this) {
        }
      virtual ~Segment() {
        if (this->next_) {
          delete this->next_;
        }
      }
       // means default, but g++ 4.6.3 does not allow "= default"
      uint8_t flags() const { return this->flags_; }
      uint32_t seq() const { return this->seq_; }
      void append(Segment *seg) {
        this->tail_->next_ = seg;
        this->tail_ = seg;
      }
      Segment *next() {
        return this->next_;
      }
    };

    class Stream {
     private:
      bool has_base_seq_;
      uint32_t base_seq_;
      uint32_t next_seq_;
      uint32_t ack_;
      uint32_t win_size_;
      byte_t *addr_;
      size_t addr_len_;
      uint16_t port_;
      uint64_t tx_size_;

     public:
      Stream(const byte_t* addr, size_t addr_len, uint16_t port) :
          has_base_seq_(false), base_seq_(0), next_seq_(0),
          addr_len_(addr_len), port_(port), tx_size_(0) {
        this->addr_ = static_cast<byte_t*>(::malloc(this->addr_len_));
        ::memcpy(this->addr_, addr, this->addr_len_);
      }
      ~Stream() {
        free(this->addr_);
      }

      uint32_t next_seq() const {
        return this->next_seq_;
      }

      uint32_t tx_size() const {
        return this->tx_size_;
      }

      bool is_src(const Property& p) {
        size_t src_len;
        const byte_t* src_addr = p.src_addr(&src_len);
        const uint16_t src_port = p.src_port();
        return this->match(src_addr, src_len, src_port);
      }
      bool in_window(uint32_t seq) {
        const uint32_t rel_seq = seq - this->base_seq_;
        const uint32_t rel_ack = this->ack_ - this->base_seq_;
        debug(DBG_SEQ, "seq:%u, next:%u, win:%u, ack:%u", rel_seq,
         this->next_seq_, this->win_size_, rel_ack);
        // TODO(m-mizutani): need to handle TCP option Window Scale properly
        return true;
      }
      inline uint32_t to_rel_seq(uint32_t seq) {
        return seq - this->base_seq_;
      }

      void set_base_seq(uint32_t seq, size_t seg_len) {
        this->has_base_seq_ = true;
        this->base_seq_ = seq;
        this->next_seq_ = 1 + seg_len;
      }

      void inc_seq(uint32_t step = 1) {
        this->next_seq_ += step;
      }

      inline bool match(const byte_t* addr, size_t addr_len, uint16_t port) {
        return (this->addr_len_ == addr_len && this->port_ == port &&
                ::memcmp(addr, this->addr_, addr_len) == 0);
      }

      bool send(uint8_t flags, uint32_t seq, uint32_t ack, size_t data_len) {
        if (!this->has_base_seq_) {
          return true;
        }

        auto f = flag2str(flags);
        const uint32_t rel_seq = seq - this->base_seq_;
        debug(DBG_SEQ, "(%p) %s seq: %u, next: %u > %zu", this,
              f.c_str(), rel_seq, this->next_seq_, data_len);

        if (this->next_seq_ == rel_seq) {
          this->next_seq_ += data_len;
        } else {
          debug(DBG_SEQ, "seq/ack mismatched");
          return false;
        }

        return true;
      }

      void recv(uint32_t ack, uint32_t win_size) {
        this->ack_ = ack;
        this->win_size_ = win_size;
        return;
      }
    } *client_, *server_, *closing_;

    TCP *tcp_;
    uint64_t id_;
    Status status_;
    struct timeval ts_init_;
    struct timeval ts_estb_;
    struct timeval ts_rtt_;
    tb::Buffer *buf_;
    std::map<uint32_t, Segment*> seg_map_;

   public:
    explicit Session(const Property& p, TCP *tcp, uint64_t ssn_id) :
        closing_(nullptr), tcp_(tcp), id_(ssn_id), status_(NONE),
        buf_(nullptr) {
      size_t src_len, dst_len;
      const byte_t* src_addr = p.src_addr(&src_len);
      const byte_t* dst_addr = p.dst_addr(&dst_len);
      const uint16_t src_port = p.src_port();
      const uint16_t dst_port = p.dst_port();
      this->client_ = new Stream(src_addr, src_len, src_port);
      this->server_ = new Stream(dst_addr, dst_len, dst_port);
    }
    ~Session() {
      delete this->client_;
      delete this->server_;
      delete this->buf_;
      for (auto it : this->seg_map_) {
        delete it.second;
      }
    }

    uint64_t id() const { return this->id_; }
    Status status() const { return this->status_; }

    Status trans_state(uint8_t flags, Stream* sender, uint32_t seq,
                       size_t seg_len, const struct timeval& tv) {
      Status new_status = NONE;

      switch (this->status_) {
        case NONE:
          if (flags == SYN && sender == this->client_) {
            debug(DBG_STAT, "%p: SYN", this);
            new_status = this->status_ = SYN_SENT;
            ::memcpy(&this->ts_init_, &tv, sizeof(this->ts_init_));
            sender->set_base_seq(seq, seg_len);
          }
          break;

        case SYN_SENT:
          if (flags == (SYN|ACK) && sender == this->server_) {
            debug(DBG_STAT, "%p: SYN-ACK", this);
            new_status = this->status_ = SYNACK_SENT;
            sender->set_base_seq(seq, seg_len);
          }
          break;

        case SYNACK_SENT:
          if (flags == ACK && sender == this->client_) {
            debug(DBG_STAT, "%p: ACK, ESTABLISHED", this);
            new_status = this->status_ = ESTABLISHED;
            ::memcpy(&this->ts_estb_, &tv, sizeof(this->ts_estb_));
            timersub(&this->ts_estb_, &this->ts_init_, &this->ts_rtt_);
          }
          break;

        case ESTABLISHED:
          if ((flags & FIN) > 0) {
            debug(DBG_STAT, "%p: FIN", this);
            new_status = this->status_ = CLOSING;
            this->closing_ = sender;
            sender->inc_seq();
          }
          break;

        case CLOSING:
          if ((flags & FIN) > 0 && this->closing_ != sender) {
            debug(DBG_STAT, "%p: CLOSED", this);
            new_status = this->status_ = CLOSED;
            sender->inc_seq();
          }
          break;

        case CLOSED:
        const std::string s = flag2str(flags);
        debug(false, "already closed: %p -> %s", this, s.c_str());
          break;  // pass
      }

      return new_status;
    }

    bool decode_stream(Property* p, uint8_t flags, uint32_t seq, uint32_t ack,
                size_t seg_len, const void* seg_ptr, uint16_t win_size,
                Stream* sender, Stream* recver) {
      if (!sender->send(flags, seq, ack, seg_len)) {
        if (sender->in_window(seq)) {
          uint32_t rel_seq = sender->to_rel_seq(seq);
          auto it = this->seg_map_.find(rel_seq);
          Segment *seg = new Segment(seg_ptr, seg_len, seq, flags);
          if (it == this->seg_map_.end()) {
            this->seg_map_.insert(std::make_pair(rel_seq, seg));
            debug(DBG_SEQ, "in window!, inserted");
          } else {
            it->second->append(seg);
          }
        } else {
          debug(DBG_SEQ, "out of window");
        }

        return false;  // Invalid sequence
      }
      recver->recv(ack, win_size);

      // if (this->seg_map_.size() > 0) {
      Status new_state = this->trans_state(flags, sender, seq, seg_len,
                                           p->tv());
      if (new_state == ESTABLISHED) {
        p->push_event(this->tcp_->ev_estb());
        const uint32_t ts = (this->ts_rtt_.tv_sec * 1000000) +
                            this->ts_rtt_.tv_usec;
        p->retain_value(this->tcp_->p_rtt_3wh())->cpy(&ts, sizeof(ts),
                                                      Value::LITTLE);
      }
      if (new_state == CLOSED) {
        p->push_event(this->tcp_->ev_close());
      }

      if (this->buf_) {
        this->buf_->append(seg_ptr, seg_len);
        p->retain_value(this->tcp_->p_data())->set(this->buf_->ptr(),
                                                   this->buf_->len());
      } else {
        p->retain_value(this->tcp_->p_data())->set(seg_ptr, seg_len);
      }

      if (this->seg_map_.size() > 0) {
        auto it = this->seg_map_.find(sender->next_seq());
        debug(DBG_REASS, "next > %u, it > %u", sender->next_seq(), it->first);
        if (it != this->seg_map_.end()) {
          auto seg = it->second;
          debug(DBG_REASS, "=== matched stored segment");
          if (this->buf_ == nullptr) {
            this->buf_ = new tb::Buffer();
            this->buf_->append(seg_ptr, seg_len);
          }

          this->seg_map_.erase(it);
          Segment *tgt = seg;
          while (tgt) {
            this->decode_stream(p, tgt->flags(), tgt->seq(), ack, tgt->len(),
                                tgt->ptr(), win_size, sender, recver);
            tgt = tgt->next();
          }
          delete seg;
        }
      }

      return true;
    }

    void decode(Property* p, uint8_t flags, uint32_t seq, uint32_t ack,
                size_t seg_len, const void* seg_ptr, uint16_t win_size) {
      if (this->buf_) {
        delete this->buf_;
        this->buf_ = nullptr;
      }

      Stream *sender, *recver;
      if (this->client_->is_src(*p)) {
        sender = this->client_;
        recver = this->server_;
      } else {
        sender = this->server_;
        recver = this->client_;
      }

      this->decode_stream(p, flags, seq, ack, seg_len, seg_ptr, win_size,
                          sender, recver);
      uint32_t tx_c = this->server_->tx_size();  // from Server to Client
      uint32_t tx_s = this->client_->tx_size();  // fron Client to Server
      p->retain_value(this->tcp_->p_tx_server())->cpy(&tx_s, sizeof(tx_s),
                                                      Value::LITTLE);
      p->retain_value(this->tcp_->p_tx_client())->cpy(&tx_c, sizeof(tx_c),
                                                      Value::LITTLE);
    }

    static void make_key(const Property& p, tb::HashKey* key) {
      size_t src_len, dst_len;
      const byte_t* src_addr = p.src_addr(&src_len);
      const byte_t* dst_addr = p.dst_addr(&dst_len);
      const uint16_t src_port = p.src_port();
      const uint16_t dst_port = p.dst_port();
      debug(false, "port: %d -> %d", src_port, dst_port);
      assert(src_len == dst_len);
      const size_t keylen = src_len + dst_len +
                            sizeof(src_port) + sizeof(dst_port);
      key->clear();
      key->resize(keylen);

      int rc = ::memcmp(src_addr, dst_addr, src_len);
      if (rc > 0 || (rc == 0 && src_port > dst_port)) {
        key->append(src_addr, src_len);
        key->append(&src_port, sizeof(src_port));
        key->append(dst_addr, dst_len);
        key->append(&dst_port, sizeof(dst_port));
      } else {
        key->append(dst_addr, dst_len);
        key->append(&dst_port, sizeof(dst_port));
        key->append(src_addr, src_len);
        key->append(&src_port, sizeof(src_port));
      }

      key->finalize();
    }
  };

  tb::LruHash<Session*> ssn_table_;


 public:
  TCP() : ssn_count_(0), curr_ts_(0), init_ts_(false),
    ssn_table_(3600, 0xffff) {
    this->p_src_port_ = this->define_param("src_port",
                                        value::PortNumber::new_value);
    this->p_dst_port_ = this->define_param("dst_port",
                                        value::PortNumber::new_value);

#define DEFINE_PARAM(name) \
    this->p_ ## name ## _ = this->define_param(#name);

    DEFINE_PARAM(seq);
    DEFINE_PARAM(ack);
    DEFINE_PARAM(offset);
    DEFINE_PARAM(flags);
    DEFINE_PARAM(window);
    DEFINE_PARAM(chksum);
    DEFINE_PARAM(urgptr);

    // Flags
    DEFINE_PARAM(flag_fin);
    DEFINE_PARAM(flag_syn);
    DEFINE_PARAM(flag_rst);
    DEFINE_PARAM(flag_push);
    DEFINE_PARAM(flag_ack);
    DEFINE_PARAM(flag_urg);
    DEFINE_PARAM(flag_ece);
    DEFINE_PARAM(flag_cwr);

    // Option
    DEFINE_PARAM(optdata);

    // Segment
    DEFINE_PARAM(segment);
    DEFINE_PARAM(data);
    DEFINE_PARAM(rtt_3wh);
    DEFINE_PARAM(tx_server);
    DEFINE_PARAM(tx_client);

    this->p_ssn_id_ = this->define_param("id");
    this->ev_new_ = this->define_event("new_session");
    this->ev_estb_ = this->define_event("established");
    this->ev_close_ = this->define_event("closed");
  }

  mod_id mod_tcpssn_;
  void setup() {
    this->mod_tcpssn_ = this->lookup_module("TCPSession");
  }

  // ------------------------------------------
  // Getter

  const EventDef *ev_estb() const     { return this->ev_estb_; }
  const ParamDef* p_data() const      { return this->p_data_; }
  const ParamDef* p_rtt_3wh() const   { return this->p_rtt_3wh_; }
  const ParamDef* p_tx_server() const { return this->p_tx_server_; }
  const ParamDef* p_tx_client() const { return this->p_tx_client_; }
  const EventDef* ev_close() const    { return this->ev_close_; }


  mod_id decode(Payload* pd, Property* prop) {
    auto hdr = reinterpret_cast<const struct tcp_header*>
               (pd->retain(sizeof(struct tcp_header)));
    if (hdr == nullptr) {   // Not enough packet size.
      return Module::NONE;
    }

    // ----------------------------------------
    // TCP header processing

    prop->set_src_port(ntohs(hdr->src_port_));
    prop->set_dst_port(ntohs(hdr->dst_port_));

#define SET_PROP_FROM_HDR(NAME)                                         \
    prop->retain_value(this->p_ ## NAME)->set(&(hdr->NAME), sizeof(hdr->NAME));

    SET_PROP_FROM_HDR(src_port_);
    SET_PROP_FROM_HDR(dst_port_);
    SET_PROP_FROM_HDR(seq_);
    SET_PROP_FROM_HDR(ack_);

    const uint8_t offset = (hdr->offset_ & 0xf0) >> 2;
    prop->retain_value(this->p_offset_)->cpy(&offset, sizeof(offset));

    SET_PROP_FROM_HDR(offset_);
    SET_PROP_FROM_HDR(flags_);
    SET_PROP_FROM_HDR(window_);
    SET_PROP_FROM_HDR(chksum_);
    SET_PROP_FROM_HDR(urgptr_);

#define SET_FLAGS(FNAME, PNAME)                                         \
    {                                                                   \
      byte_t f = ((hdr->flags_ & (FNAME)) > 0);                         \
      prop->retain_value(this->p_flag_ ## PNAME ## _)->cpy(&f, sizeof(f)); \
    }

    SET_FLAGS(FIN, fin);
    SET_FLAGS(SYN, syn);
    SET_FLAGS(RST, rst);
    SET_FLAGS(PUSH, push);
    SET_FLAGS(ACK, ack);
    SET_FLAGS(URG, urg);
    SET_FLAGS(ECE, ece);
    SET_FLAGS(CWR, cwr);

    // Set option data.
    const size_t optlen = offset - sizeof(tcp_header);
    if (optlen > 0) {
      const byte_t* opt = pd->retain(optlen);
      if (opt == nullptr) {
        return Module::NONE;
      }

      prop->retain_value(this->p_optdata_)->set(opt, optlen);
    }

    const size_t seg_len = pd->length();
    const byte_t* seg_ptr = nullptr;

    // Set segment data.
    if (seg_len > 0) {
      seg_ptr = pd->retain(seg_len);
      prop->retain_value(this->p_segment_)->set(seg_ptr, seg_len);
    }

    // ----------------------------------------
    // TCP session management
    tb::HashKey key;

    time_t ts = prop->ts();
    if (this->curr_ts_ < ts) {
      time_t diff = ts - this->curr_ts_;
      this->curr_ts_ = ts;
      if (this->init_ts_) {
        this->ssn_table_.step(diff);
      } else {
        this->init_ts_ = true;
      }
    }

    static const bool DBG_SSN = false;
    while (this->ssn_table_.has_expired()) {
      Session* old_ssn = this->ssn_table_.pop_expired();
      debug(DBG_SSN, "expired: %p", old_ssn);
      delete old_ssn;
    }

    uint8_t flags = (hdr->flags_ & (FIN | SYN | RST | ACK));
    flags &= (SYN|ACK|FIN|RST);
    uint32_t seq = ntohl(hdr->seq_);
    uint32_t ack = ntohl(hdr->ack_);
    uint16_t win = ntohs(hdr->window_);

    Session::make_key(*prop, &key);
    auto node = this->ssn_table_.get(key);

    Session* ssn = nullptr;
    if (node.is_null()) {
      this->ssn_count_ += 1;
      ssn = new Session(*prop, this, this->ssn_count_);
      this->ssn_table_.put(300, key, ssn);
      debug(DBG_SSN, "new session: %p", ssn);
      prop->push_event(this->ev_new_);
    } else {
      ssn = node.data();
      debug(DBG_SSN, "existing session: %p", ssn);
    }

    if (ssn) {
      debug(DBG, "ssn = %p", ssn);
      const uint64_t ssn_id = ssn->id();
      prop->retain_value(this->p_ssn_id_)->cpy(&ssn_id, sizeof(ssn_id));
      ssn->decode(prop, flags, seq, ack, seg_len, seg_ptr, win);
    }

    return Module::NONE;
  }
};

INIT_MODULE(TCP);

}   // namespace pm
