//
// Copyright 2019 Ettus Research, a National Instruments Company
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/utils/log.hpp>
#include <uhdlib/transport/dpdk_simple.hpp>
#include <uhdlib/transport/uhd-dpdk.h>
#include <arpa/inet.h>

namespace uhd { namespace transport {

namespace {
    constexpr uint64_t USEC = 1000000;
    // Non-data fields are headers (Ethernet + IPv4 + UDP) + CRC
    constexpr size_t DPDK_SIMPLE_NONDATA_SIZE = 14 + 20 + 8 + 4;
}

class dpdk_simple_impl : public dpdk_simple {
public:
    dpdk_simple_impl(struct uhd_dpdk_ctx &ctx, const std::string &addr,
                     const std::string &port, bool filter_bcast)
    {
        UHD_ASSERT_THROW(ctx.is_init_done());

        // Get NIC that can route to addr
        int port_id = ctx.get_route(addr);
        UHD_ASSERT_THROW(port_id >= 0);

        _port_id = port_id;
        uint32_t dst_ipv4 = (uint32_t) inet_addr(addr.c_str());
        uint16_t dst_port = htons(std::stoi(port, NULL, 0));

        struct uhd_dpdk_sockarg_udp sockarg = {
            .is_tx = false,
            .filter_bcast = filter_bcast,
            .local_port = 0,
            .remote_port = dst_port,
            .dst_addr = dst_ipv4,
            .num_bufs = 1
        };
        _rx_sock = uhd_dpdk_sock_open(_port_id, UHD_DPDK_SOCK_UDP, &sockarg);
        UHD_ASSERT_THROW(_rx_sock != nullptr);

        // Backfill the local port, in case it was auto-assigned
        uhd_dpdk_udp_get_info(_rx_sock, &sockarg);
        sockarg.is_tx = true;
        sockarg.remote_port = dst_port;
        sockarg.dst_addr = dst_ipv4;
        sockarg.num_bufs = 1;
        _tx_sock = uhd_dpdk_sock_open(_port_id, UHD_DPDK_SOCK_UDP, &sockarg);
        UHD_ASSERT_THROW(_tx_sock != nullptr);
        UHD_LOG_TRACE("DPDK", "Created simple transports between " << addr << ":"
                               << ntohs(dst_port) << " and NIC(" << _port_id
                               << "):" << ntohs(sockarg.local_port));
    }

    ~dpdk_simple_impl(void)
    {
        if (_rx_mbuf)
            uhd_dpdk_free_buf(_rx_mbuf);
        if (_tx_mbuf)
            uhd_dpdk_free_buf(_tx_mbuf);
    }

    /*!
     * Request a single send buffer of specified size.
     *
     * \param buf a pointer to place to write buffer location
     * \return the maximum length of the buffer
     */
    size_t get_tx_buf(void** buf)
    {
        UHD_ASSERT_THROW(!_tx_mbuf);
        int bufs = uhd_dpdk_request_tx_bufs(_tx_sock, &_tx_mbuf, 1, 0);
        if (bufs != 1 || !_tx_mbuf) {
            *buf = nullptr;
            return 0;
        }
        *buf = uhd_dpdk_buf_to_data(_tx_sock, _tx_mbuf);
        return _mtu - DPDK_SIMPLE_NONDATA_SIZE;
    }

    /*!
     * Send and release outstanding buffer
     *
     * \param length bytes of data to send
     * \return number of bytes sent (releases buffer if sent)
     */
    size_t send(size_t length)
    {
        UHD_ASSERT_THROW(_tx_mbuf)
        _tx_mbuf->pkt_len = length;
        _tx_mbuf->data_len = length;
        int num_tx = uhd_dpdk_send(_tx_sock, &_tx_mbuf, 1);
        if (num_tx == 0)
            return 0;
        _tx_mbuf = nullptr;
        return length;
    }

    /*!
     * Receive a single packet.
     * Buffer provided by transport (must be freed before next operation).
     *
     * \param buf a pointer to place to write buffer location
     * \param timeout the timeout in seconds
     * \return the number of bytes received or zero on timeout
     */
    size_t recv(void **buf, double timeout)
    {
        UHD_ASSERT_THROW(!_rx_mbuf);
        int bufs = uhd_dpdk_recv(_rx_sock, &_rx_mbuf, 1, (int) (timeout*USEC));
        if (bufs != 1 || _rx_mbuf == nullptr) {
            *buf = nullptr;
            return 0;
        }
        if ((_tx_mbuf->ol_flags & PKT_RX_IP_CKSUM_MASK) == PKT_RX_IP_CKSUM_BAD) {
            uhd_dpdk_free_buf(_rx_mbuf);
            _rx_mbuf = nullptr;
            return 0;
        }
        uhd_dpdk_get_src_ipv4(_rx_sock, _rx_mbuf, &_last_recv_addr);
        *buf = uhd_dpdk_buf_to_data(_rx_sock, _rx_mbuf);
        return uhd_dpdk_get_len(_rx_sock, _rx_mbuf);
    }

    /*!
     * Return/free receive buffer
     * Can also use to free un-sent TX bufs
     */
    void put_rx_buf(void)
    {
        UHD_ASSERT_THROW(_rx_mbuf)
        uhd_dpdk_free_buf(_rx_mbuf);
    }

    /*!
     * Get the last IP address as seen by recv().
     * Only use this with the broadcast socket.
     */
    std::string get_recv_addr(void)
    {
        char addr_str[INET_ADDRSTRLEN];
        struct in_addr ipv4_addr;
        ipv4_addr.s_addr = _last_recv_addr;
        inet_ntop(AF_INET, &ipv4_addr, addr_str, sizeof(addr_str));
        return std::string(addr_str);
    }

    /*!
     * Get the IP address for the destination
     */
    std::string get_send_addr(void)
    {
        struct in_addr ipv4_addr;
        int status = uhd_dpdk_get_ipv4_addr(_port_id, &ipv4_addr.s_addr, nullptr);
        UHD_ASSERT_THROW(status);
        char addr_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ipv4_addr, addr_str, sizeof(addr_str));
        return std::string(addr_str);
    }
private:
    unsigned int _port_id;
    size_t _mtu;
    struct uhd_dpdk_socket *_tx_sock;
    struct rte_mbuf *_tx_mbuf = nullptr;
    struct uhd_dpdk_socket *_rx_sock;
    struct rte_mbuf *_rx_mbuf = nullptr;
    uint32_t _last_recv_addr;
};

dpdk_simple::~dpdk_simple(void) {}

/***********************************************************************
 * DPDK simple transport public make functions
 **********************************************************************/
dpdk_simple::sptr dpdk_simple::make_connected(
    struct uhd_dpdk_ctx &ctx, const std::string &addr, const std::string &port
){
    return sptr(new dpdk_simple_impl(ctx, addr, port, true));
}

dpdk_simple::sptr dpdk_simple::make_broadcast(
    struct uhd_dpdk_ctx &ctx, const std::string &addr, const std::string &port
){
    return sptr(new dpdk_simple_impl(ctx, addr, port, false));
}
}} // namespace uhd::transport


