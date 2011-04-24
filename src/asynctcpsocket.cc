/*
 * libjingle
 * Copyright 2004--2010, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "asynctcpsocket.h"

#include <cstring>

#include "byteorder.h"
#include "common.h"
#include "logging.h"

#ifdef POSIX
#include <errno.h>
#endif  // POSIX

namespace txmpp {

static const size_t MAX_PACKET_SIZE = 64 * 1024;

typedef uint16 PacketLength;
static const size_t PKT_LEN_SIZE = sizeof(PacketLength);

static const size_t BUF_SIZE = MAX_PACKET_SIZE + PKT_LEN_SIZE;

static const int LISTEN_BACKLOG = 5;

AsyncTCPSocket* AsyncTCPSocket::Create(SocketFactory* factory, bool listen) {
  AsyncSocket* sock = factory->CreateAsyncSocket(SOCK_STREAM);
  // This will still return a socket even if we failed to listen on
  // it. It is neccessary because even if we can't accept new
  // connections on this socket, the corresponding port is still
  // useful for outgoing connections.
  //
  // TODO: It might be better to pass listen() error to the
  // upper layer and let it handle the problem.
  return (sock) ? new AsyncTCPSocket(sock, listen) : NULL;
}

AsyncTCPSocket::AsyncTCPSocket(AsyncSocket* socket, bool listen)
    : AsyncPacketSocket(socket),
      listen_(listen),
      insize_(BUF_SIZE),
      inpos_(0),
      outsize_(BUF_SIZE),
      outpos_(0) {
  inbuf_ = new char[insize_];
  outbuf_ = new char[outsize_];

  ASSERT(socket_ != NULL);
  socket_->SignalConnectEvent.connect(this, &AsyncTCPSocket::OnConnectEvent);
  socket_->SignalReadEvent.connect(this, &AsyncTCPSocket::OnReadEvent);
  socket_->SignalWriteEvent.connect(this, &AsyncTCPSocket::OnWriteEvent);
  socket_->SignalCloseEvent.connect(this, &AsyncTCPSocket::OnCloseEvent);

  if (listen_) {
    if (socket_->Listen(LISTEN_BACKLOG) < 0) {
      LOG(LS_ERROR) << "Listen() failed with error " << socket_->GetError();
    }
  }
}

AsyncTCPSocket::~AsyncTCPSocket() {
  delete [] inbuf_;
  delete [] outbuf_;
}

int AsyncTCPSocket::Send(const void *pv, size_t cb) {
  if (cb > MAX_PACKET_SIZE) {
    socket_->SetError(EMSGSIZE);
    return -1;
  }

  // If we are blocking on send, then silently drop this packet
  if (outpos_)
    return static_cast<int>(cb);

  PacketLength pkt_len = HostToNetwork16(static_cast<PacketLength>(cb));
  memcpy(outbuf_, &pkt_len, PKT_LEN_SIZE);
  memcpy(outbuf_ + PKT_LEN_SIZE, pv, cb);
  outpos_ = PKT_LEN_SIZE + cb;

  int res = Flush();
  if (res <= 0) {
    // drop packet if we made no progress
    outpos_ = 0;
    return res;
  }

  // We claim to have sent the whole thing, even if we only sent partial
  return static_cast<int>(cb);
}

int AsyncTCPSocket::SendTo(const void *pv, size_t cb,
                           const SocketAddress& addr) {
  if (addr == GetRemoteAddress())
    return Send(pv, cb);

  ASSERT(false);
  socket_->SetError(ENOTCONN);
  return -1;
}

int AsyncTCPSocket::SendRaw(const void * pv, size_t cb) {
  if (outpos_ + cb > outsize_) {
    socket_->SetError(EMSGSIZE);
    return -1;
  }

  memcpy(outbuf_ + outpos_, pv, cb);
  outpos_ += cb;

  return Flush();
}

void AsyncTCPSocket::ProcessInput(char * data, size_t& len) {
  SocketAddress remote_addr(GetRemoteAddress());

  while (true) {
    if (len < PKT_LEN_SIZE)
      return;

    PacketLength pkt_len;
    memcpy(&pkt_len, data, PKT_LEN_SIZE);
    pkt_len = NetworkToHost16(pkt_len);

    if (len < PKT_LEN_SIZE + pkt_len)
      return;

    SignalReadPacket(this, data + PKT_LEN_SIZE, pkt_len, remote_addr);

    len -= PKT_LEN_SIZE + pkt_len;
    if (len > 0) {
      memmove(data, data + PKT_LEN_SIZE + pkt_len, len);
    }
  }
}

int AsyncTCPSocket::Flush() {
  int res = socket_->Send(outbuf_, outpos_);
  if (res <= 0) {
    return res;
  }
  if (static_cast<size_t>(res) <= outpos_) {
    outpos_ -= res;
  } else {
    ASSERT(false);
    return -1;
  }
  if (outpos_ > 0) {
    memmove(outbuf_, outbuf_ + res, outpos_);
  }
  return res;
}

void AsyncTCPSocket::OnConnectEvent(AsyncSocket* socket) {
  SignalConnect(this);
}

void AsyncTCPSocket::OnReadEvent(AsyncSocket* socket) {
  ASSERT(socket == socket_);

  if (listen_) {
    txmpp::SocketAddress address;
    txmpp::AsyncSocket* new_socket = socket->Accept(&address);
    if (!new_socket) {
      // TODO: Do something better like forwarding the error
      // to the user.
      LOG(LS_ERROR) << "TCP accept failed with error " << socket_->GetError();
      return;
    }

    SignalNewConnection(this, new AsyncTCPSocket(new_socket, false));

    // Prime a read event in case data is waiting.
    new_socket->SignalReadEvent(new_socket);
  } else {
    int len = socket_->Recv(inbuf_ + inpos_, insize_ - inpos_);
    if (len < 0) {
      // TODO: Do something better like forwarding the error to the user.
      if (!socket_->IsBlocking()) {
        LOG(LS_ERROR) << "Recv() returned error: " << socket_->GetError();
      }
      return;
    }

    inpos_ += len;

    ProcessInput(inbuf_, inpos_);

    if (inpos_ >= insize_) {
      LOG(LS_ERROR) << "input buffer overflow";
      ASSERT(false);
      inpos_ = 0;
    }
  }
}

void AsyncTCPSocket::OnWriteEvent(AsyncSocket* socket) {
  ASSERT(socket == socket_);

  if (outpos_ > 0) {
    Flush();
  }
}

void AsyncTCPSocket::OnCloseEvent(AsyncSocket* socket, int error) {
  SignalClose(this, error);
}

} // namespace txmpp
