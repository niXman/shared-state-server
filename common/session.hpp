
// ----------------------------------------------------------------------------
//                              Apache License
//                        Version 2.0, January 2004
//                     http://www.apache.org/licenses/
//
// This file is part of shared-state-server(https://github.com/niXman/shared-state-server) project.
//
// This was a test task for implementing multithreaded Shared-State server using asio.
//
// Copyright (c) 2023 niXman (github dot nixman dog pm.me). All rights reserved.
// ----------------------------------------------------------------------------

#ifndef __shared_state_server__session_hpp__included
#define __shared_state_server__session_hpp__included

#include "utils.hpp"
#include "intrusive_base.hpp"
#include "intrusive_ptr.hpp"
#include "object_pool.hpp"
#include "string_buffer.hpp"

#include <boost/intrusive/list_hook.hpp>

/**********************************************************************************************************************/

struct session: boost::intrusive::list_base_hook<>, intrusive_base<session> {
    using session_ptr = intrusive_ptr<session>;

    session(tcp::socket sock, std::size_t max_size, std::size_t inactivity_time, buffers_pool &pool)
        :m_sock{std::move(sock)}
        ,m_inactivity_timer{m_sock.get_executor(), std::chrono::milliseconds{m_inactivity_time}}
        ,m_on_stop{false}
        ,m_max_size{max_size}
        ,m_inactivity_time{inactivity_time}
        ,m_pool{pool}
    { m_sock.set_option(tcp::no_delay{true}); }
    virtual ~session() = default;

    // may be called from any thread
    // ReadedCB's signature: bool(shared_buf, holder_ptr)
    // ErrorCB's signature: void(error_handler_info)
    template<typename ReadedCB, typename ErrorCB>
    void start(ReadedCB readed_cb, ErrorCB error_cb, session_ptr holder = {}) {
        auto lambda = [this, readed_cb=std::move(readed_cb)
            ,error_cb=std::move(error_cb), holder=std::move(holder)]
        () mutable
        { start_impl(std::move(readed_cb), std::move(error_cb), make_buffer(m_pool), std::move(holder)); };

        ba::post(
             m_sock.get_executor()
            ,std::move(lambda)
        );
    }

    auto stop() {
        return ba::post(
             m_sock.get_executor()
            ,ba::use_future([this](){ stop_impl(); })
        );
    }

    // may be called from any thread
    // SentCB's signature: void(bool) - true, if the message was sent successfully
    // ErrorCB's signature: void(error_handler_info)
    template<typename SentCB, typename ErrorCB>
    void send(SentCB sent_cb, ErrorCB error_cb, shared_buffer msg, bool disconnect, session_ptr holder) {
        auto lambda = [this, sent_cb=std::move(sent_cb), error_cb=std::move(error_cb)
            ,msg=std::move(msg), disconnect, holder=std::move(holder)]
        () mutable
        { send_impl(std::move(sent_cb), std::move(error_cb), std::move(msg), disconnect, std::move(holder)); };

        ba::post(
             m_sock.get_executor()
            ,std::move(lambda)
        );
    }

    auto& get_socket() { return m_sock; }
    auto endpoint() const { return m_sock.remote_endpoint(); }

private:
    template<typename SentCB, typename ErrorCB>
    void send_impl(SentCB sent_cb, ErrorCB error_cb, shared_buffer msg, bool disconnect, session_ptr holder) {
        auto *str = msg.get();
        auto lambda = [this, sent_cb=std::move(sent_cb), error_cb=std::move(error_cb)
            ,msg=std::move(msg), disconnect, holder=std::move(holder)]
        (const bs::error_code &ec, std::size_t) mutable {
            if ( !m_on_stop && ec ) {
                CALL_ERROR_HANDLER(error_cb, MAKE_ERROR_INFO("session", ec));

                sent_cb(false);

                return;
            }

            sent_cb(true);

            if ( disconnect ) {
                stop();
            }
        };

        ba::async_write(
             m_sock
            ,ba::buffer(str->string())
            ,std::move(lambda)
        );
    }

    void stop_impl() {
        if ( m_on_stop ) { return; }

        m_on_stop = true;
        bs::error_code ec;
        m_sock.shutdown(tcp::socket::shutdown_both, ec);
        ec = bs::error_code{};
        m_sock.close(ec);
        ec = bs::error_code{};
        m_inactivity_timer.cancel(ec);
    }

    void start_inactivity_timer(session_ptr holder) {
        m_inactivity_timer.async_wait(
            [this, holder=std::move(holder)]
            (bs::error_code ec) mutable
            { on_inactivity_timer_timeout(ec); }
        );
    }
    void on_inactivity_timer_timeout(bs::error_code ec) {
        if ( ec == boost::asio::error::operation_aborted ) { return; }

        stop();
    }

    template<typename ReadedCB, typename ErrorCB>
    void start_impl(ReadedCB readed_cb, ErrorCB error_cb, shared_buffer buf, session_ptr holder) {
        if ( m_inactivity_time ) { start_inactivity_timer(holder); }

        start_read(std::move(readed_cb), std::move(error_cb), std::move(buf), std::move(holder));
    }
    template<typename ReadedCB, typename ErrorCB>
    void start_read(ReadedCB readed_cb, ErrorCB error_cb, shared_buffer buf, session_ptr holder) {
        auto *ptr = buf.get();
        auto lambda = [this, readed_cb=std::move(readed_cb), error_cb=std::move(error_cb)
            ,buf=std::move(buf), holder=std::move(holder)]
        (const bs::error_code& ec, std::size_t rd) mutable
        { on_readed(std::move(readed_cb), std::move(error_cb), std::move(buf), ec, rd, std::move(holder)); };

        ba::async_read_until(
             m_sock
            ,ba::dynamic_buffer(ptr->string(), m_max_size)
            ,'\n'
            ,std::move(lambda)
        );
    }
    template<typename ReadedCB, typename ErrorCB>
    void on_readed(
         ReadedCB readed_cb
        ,ErrorCB error_cb
        ,shared_buffer buf
        ,bs::error_code ec
        ,std::size_t rd
        ,session_ptr holder)
    {
        if ( !m_on_stop && ec ) {
            CALL_ERROR_HANDLER(error_cb, MAKE_ERROR_INFO("session", ec));

            return;
        }

        if ( m_inactivity_time ) {
            if ( m_inactivity_timer.expires_after(std::chrono::milliseconds{m_inactivity_time}) > 0 ) {
                start_inactivity_timer(holder);
            } else {
                ec = ba::error::timed_out;
                CALL_ERROR_HANDLER(error_cb, MAKE_ERROR_INFO("session", ec));

                return;
            }
        }

        auto str = make_buffer(m_pool, buf->data(), buf->data() + rd);
        buf->erase(0, rd);

        if ( readed_cb(std::move(str), holder) ) {
            start_read(std::move(readed_cb), std::move(error_cb), std::move(buf), std::move(holder));
        } else {
            m_inactivity_timer.cancel();
        }
    }

private:
    tcp::socket m_sock;
    ba::steady_timer m_inactivity_timer;
    bool m_on_stop;
    std::size_t m_max_size;
    std::size_t m_inactivity_time;
    buffers_pool &m_pool;
};

using sessions_pool = object_pool<session>;

/**********************************************************************************************************************/

#endif // __shared_state_server__session_hpp__included
