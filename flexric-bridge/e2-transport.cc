/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
// Copyright (c) 2026 Muhammad Uzair
// SPDX-License-Identifier: GPL-2.0-only

#include "e2-transport.h"

#include "ns3/log.h"
#include "ns3/system-socket.h"

namespace ns3
{
namespace oranntn
{
namespace flexric
{

NS_LOG_COMPONENT_DEFINE("E2Transport");

namespace
{

/// Common helper that frames + writes one E2 message into an open
/// SOCK_STREAM socket. Returns the number of payload bytes written
/// (excluding the 6-byte header) or -1 on error.
int
SendFramed(SystemSocket::Handle fd, const E2Message& msg)
{
    if (fd < 0)
    {
        return -1;
    }
    const auto type = static_cast<uint16_t>(msg.type);
    const auto length = static_cast<uint32_t>(msg.payload.size());
    // A single buffer preserves the message boundary on SCTP.
    std::vector<uint8_t> frame = {static_cast<uint8_t>(type >> 8),
                                  static_cast<uint8_t>(type),
                                  static_cast<uint8_t>(length >> 24),
                                  static_cast<uint8_t>(length >> 16),
                                  static_cast<uint8_t>(length >> 8),
                                  static_cast<uint8_t>(length)};
    frame.insert(frame.end(), msg.payload.begin(), msg.payload.end());
    size_t off = 0;
    while (off < frame.size())
    {
        auto n = SystemSocket::Send(fd, frame.data() + off, frame.size() - off);
        if (n <= 0)
        {
            return -1;
        }
        off += static_cast<size_t>(n);
    }
    return static_cast<int>(msg.payload.size());
}

bool
RecvAll(SystemSocket::Handle fd, uint8_t* buf, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n)
    {
        if (!SystemSocket::WaitReadable(fd, timeout_ms))
        {
            return false;
        }
        const auto r = SystemSocket::Receive(fd, buf + got, n - got);
        if (r <= 0)
        {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool
RecvFramed(SystemSocket::Handle fd, E2Message& out, uint32_t timeout_ms)
{
    if (fd < 0)
    {
        return false;
    }
    uint8_t header[6];
    if (!RecvAll(fd, header, 6, timeout_ms))
    {
        return false;
    }
    const uint16_t type = (uint16_t(header[0]) << 8) | header[1];
    const uint32_t len = (uint32_t(header[2]) << 24) | (uint32_t(header[3]) << 16) |
                         (uint32_t(header[4]) << 8) | header[5];
    if (len > 8u * 1024u * 1024u) // 8 MB sanity cap
    {
        NS_LOG_WARN("E2 frame size " << len << " > 8 MB cap");
        return false;
    }
    out.type = static_cast<E2MessageType>(type);
    out.payload.assign(len, 0);
    if (len > 0 && !RecvAll(fd, out.payload.data(), len, timeout_ms))
    {
        return false;
    }
    return true;
}

/// Common SOCK_STREAM transport — code reused by both TCP and SCTP
/// concrete classes; only the socket() arguments differ.
class StreamTransport : public E2Transport
{
  public:
    explicit StreamTransport(Protocol kind)
        : m_kind(kind)
    {
    }

    ~StreamTransport() override
    {
        Close();
    }

    bool Listen(const std::string& host, uint16_t port) override
    {
        Close();
        m_fd = SystemSocket::Listen(host, port, 4, NativeProtocol());
        return m_fd >= 0;
    }

    bool Connect(const std::string& host, uint16_t port) override
    {
        Close();
        m_fd = SystemSocket::Connect(host, port, NativeProtocol());
        return m_fd >= 0;
    }

    std::unique_ptr<E2Transport> AcceptOne(uint32_t timeout_ms) override
    {
        const auto fd = SystemSocket::Accept(m_fd, timeout_ms);
        if (fd < 0)
        {
            return nullptr;
        }
        auto out = std::make_unique<StreamTransport>(m_kind);
        out->m_fd = fd;
        return out;
    }

    int Send(const E2Message& msg) override
    {
        return SendFramed(m_fd, msg);
    }

    bool Recv(E2Message& out, uint32_t timeout_ms) override
    {
        return RecvFramed(m_fd, out, timeout_ms);
    }

    void Close() override
    {
        if (m_fd >= 0)
        {
            SystemSocket::Close(m_fd);
            m_fd = -1;
        }
    }

    Protocol Kind() const override
    {
        return m_kind;
    }

    bool IsOpen() const override
    {
        return m_fd >= 0;
    }

  private:
    SystemSocket::Protocol NativeProtocol() const
    {
        return m_kind == Protocol::tcp ? SystemSocket::Protocol::TCP : SystemSocket::Protocol::SCTP;
    }

    SystemSocket::Handle m_fd{-1};
    Protocol m_kind;
};

} // namespace

std::unique_ptr<E2Transport>
MakeE2Transport(E2Transport::Protocol p)
{
    return std::make_unique<StreamTransport>(p);
}

} // namespace flexric
} // namespace oranntn
} // namespace ns3
