
// ----------------------------------------------------------------------------
//                              Apache License
//                        Version 2.0, January 2004
//                     http://www.apache.org/licenses/
//
// This file is part of Shared-State(https://github.com/niXman/shared-state) project.
//
// This was a test task for implementing multithreaded Shared-State server using asio.
//
// Copyright (c) 2023 niXman (github dot nixman dog pm.me). All rights reserved.
// ----------------------------------------------------------------------------

#include <boost/asio.hpp>
#include <boost/uuid/detail/sha1.hpp>

namespace ba = boost::asio;
namespace bs = boost::system;
using tcp = boost::asio::ip::tcp;

#include <iostream>
#include <thread>
#include <vector>
#include <map>
#include <list>
#include <memory>

using shared_buffer = std::shared_ptr<std::string>;

shared_buffer make_buffer(std::string str = {}) {
    return std::make_shared<shared_buffer::element_type>(std::move(str));
}

shared_buffer make_buffer(const char *beg, const char *end) {
    return make_buffer(std::string{beg, end});
}

#ifndef HIDE_DEBUG_OUTPUT
#   define DEBUG_EXPR(...) __VA_ARGS__
#else
#   define DEBUG_EXPR(...)
#endif

/**********************************************************************************************************************/

struct hasher {
    hasher(ba::io_context &ioc)
        :m_ioc{ioc}
        ,m_strand{ioc}
    {}

    // CB's signature: void(std::string hash)
    template<typename CB>
    void hash(shared_buffer str, CB cb) {
        ba::post(
             m_strand
            ,[this, str=std::move(str), cb=std::move(cb)]
             () mutable
             { hash_impl(std::move(str), std::move(cb)); }
        );
    }

private:
    template<typename CB>
    void hash_impl(shared_buffer str, CB cb) {
        auto it = m_queue.insert(
             m_queue.end()
            ,{std::move(str), shared_buffer{}, std::move(cb)}
        );

        // post to io_context execution pool
        ba::post(
             m_ioc
            ,[this, it]() {
                boost::uuids::detail::sha1 sha1;
                sha1.process_bytes(it->str->data(), it->str->size());
                unsigned hash_dig[5] = {0};
                sha1.get_digest(hash_dig);

                char hash_buf[43] = {'0', 'x'};
                char *hash_buf_it = hash_buf + 2;
                auto *dig_it = std::begin(hash_dig);
                for ( ; dig_it != std::end(hash_dig); ++dig_it, hash_buf_it += 8 ) {
                    std::sprintf(hash_buf_it, "%08x", *dig_it);
                }
                auto hash = make_buffer(hash_buf, hash_buf + 42);
                // when finished - post back to our strand
                ba::post(
                     m_strand
                    ,[this, it, hash=std::move(hash)]
                     () mutable
                     { on_hashed(it, std::move(hash)); }
                );
            }
        );
    }
    // called from our strand
    template<typename It>
    void on_hashed(It it, shared_buffer hash) {
        it->hash = std::move(hash);
        if ( it == m_queue.begin() ) {
            while ( it != m_queue.end() && it->hash && !it->hash->empty() ) {
                it->cb(std::move(it->hash));
                it = m_queue.erase(it);
            }
        }
    }

private:
    ba::io_context &m_ioc;
    ba::io_context::strand m_strand;

    struct queue_item {
        shared_buffer str;
        shared_buffer hash;
        std::function<void(shared_buffer)> cb;
    };
    // `list` is used here because I need an iterator to an inserted element
    using queue_type = std::list<queue_item>;
    using queue_iterator = queue_type::iterator;

    queue_type m_queue;
};

/**********************************************************************************************************************/

struct shared_state {
    shared_state(ba::io_context &ioc)
        :m_strand{ioc}
        ,m_hasher{ioc}
    {}

    // CB's signature: void(bool updated, const byte_buffer &key, const byte_buffer &hash)
    template<typename CB>
    void update(shared_buffer key, shared_buffer val, CB cb) {
        ba::post(
             m_strand
            ,[this, key=std::move(key), val=std::move(val), cb=std::move(cb)]
             () mutable
             { calculate_hash(std::move(key), std::move(val), std::move(cb)); }
        );
    }

    // CB's signature: void(std::size_t size)
    template<typename CB>
    void get_size(CB cb) {
        ba::post(
             m_strand
            ,[this, cb=std::move(cb)]
             () mutable
             { get_size_impl(std::move(cb)); }
        );
    }

    // CB's signature: void(bool empty, const_iterator it, const byte_buffer &key, const byte_buffer &hash)
    template<typename CB>
    void get_first(CB cb) {
        ba::post(
             m_strand
            ,[this, cb=std::move(cb)]
             () mutable
             { get_first_impl(std::move(cb)); }
        );
    }
    // CB's signature: void(bool at_end, const_iterator it, const byte_buffer &key, const byte_buffer &hash)
    template<typename It, typename CB>
    void get_next(It it, CB cb) {
        ba::post(
             m_strand
            ,[this, it, cb=std::move(cb)]
             () mutable
             { get_next_impl(it, std::move(cb)); }
        );
    }

private:
    template<typename CB>
    void get_size_impl(CB cb) {
        auto size = m_map.size();
        cb(size);
    }

    template<typename CB>
    void get_first_impl(CB cb) {
        auto it = m_map.begin();
        if ( it != m_map.end() ) {
            cb(false, it, it->first, it->second);
        } else {
            cb(true, it, shared_buffer{}, shared_buffer{});
        }
    }

    template<typename It, typename CB>
    void get_next_impl(It it, CB cb) {
        it = std::next(it);
        if ( it != m_map.end() ) {
            cb(false, it, it->first, it->second);
        } else {
            cb(true, it, shared_buffer{}, shared_buffer{});
        }
    }

    template<typename CB>
    void calculate_hash(shared_buffer key, shared_buffer val, CB cb) {
        //DEBUG_EXPR(std::cout << "calculate_hash: key=" << *key << ", val=" << *val << std::endl;);

        m_hasher.hash(
             std::move(val)
            ,[this, key=std::move(key), cb=std::move(cb)]
             (shared_buffer hash) mutable {
                // back to the our strand
                ba::post(
                     m_strand
                    ,[this, hash=std::move(hash), key=std::move(key), cb=std::move(cb)]
                     () mutable
                     { hash_calculated(std::move(key), std::move(hash), std::move(cb)); }
                );
            }
        );
    }
    template<typename CB>
    void hash_calculated(shared_buffer key, shared_buffer hash, CB cb) {
        //DEBUG_EXPR(std::cout << "hash_calculated: key=" << *key << ", hash=" << *hash << std::endl;);

        // check for key
        auto it = m_map.find(key);
        if ( it == m_map.end() ) {
            auto inserted = m_map.emplace(std::move(key), std::move(hash));
            cb(true, inserted.first->first, inserted.first->second);

            return;
        }

        // check for val
        if ( *(it->second) != *hash ) {
            it->second = std::move(hash);

            cb(true, it->first, it->second);

            return;
        }

        // no changes case
        cb(false, shared_buffer{}, shared_buffer{});
    }

private:
    ba::io_context::strand m_strand;

    struct my_cmp {
        bool operator()(const shared_buffer &l, const shared_buffer &r) const {
            return *l < *r;
        }
    };
    std::map<shared_buffer, shared_buffer, my_cmp> m_map;
    hasher m_hasher;
};

/**********************************************************************************************************************/

struct session: std::enable_shared_from_this<session> {
    using holder_ptr = std::shared_ptr<session>;

    session(tcp::socket sock, shared_state &state)
        :m_sock{std::move(sock)}
        ,m_peer{m_sock.remote_endpoint()}
        ,m_state{state}
    {}

    // CB's signature: void(shared_buf)
    template<typename CB>
    void start(CB broadcast_cb, holder_ptr holder) {
        start_read(std::move(broadcast_cb), make_buffer(), std::move(holder));

        // start to sync the state
        m_state.get_first(
            [this]
            (bool empty, auto it, const shared_buffer &key, const shared_buffer &hash)
            { on_got_message(empty, it, key, hash); }
        );
    }

    // may be called from any thread
    // CB's signature: void(bs::error_code)
    template<typename CB>
    void send(shared_buffer msg, CB cb) {
        auto holder = shared_from_this();
        ba::post(
             m_sock.get_executor()
            ,[this, msg=std::move(msg), holder=std::move(holder), cb=std::move(cb)]
             () mutable
             { send_impl(std::move(msg), std::move(cb), std::move(holder)); }
        );
    }

private:
    // called from shared_state's strand
    template<typename It>
    void on_got_message(bool empty, It it, const shared_buffer &key, const shared_buffer &hash) {
        if ( !empty ) {
            //DEBUG_EXPR(std::cout << "on_got_message: empty == false" << std::endl;);

            auto str = make_buffer(*key);
            *str += ' ';
            *str += *hash;
            *str += '\n';

            //DEBUG_EXPR(std::cout << "on_got_message: msg=" << *str << std::endl;);

            ba::post(
                 m_sock.get_executor()
                ,[this, it, str=std::move(str)]
                 () mutable
                 {
                    send(
                         std::move(str)
                        ,[this, it]
                         (const bs::error_code &ec)
                         { on_get_first_sent(ec, it); }
                    );
                 }
            );
        } else {
            //DEBUG_EXPR(std::cout << "the map is empty, nothing to update!" << std::endl;);
        }
    }
    template<typename It>
    void on_get_first_sent(const bs::error_code &ec, It it) {
        if ( ec ) {
            std::cerr << "on_get_first_sent error: " << ec << std::endl;

            return;
        }

        m_state.get_next(
             it
            ,[this]
             (bool at_end, auto it, const shared_buffer &key, const shared_buffer &hash)
             { on_got_message(at_end, it, key, hash); }
        );
    }

private:
    template<typename CB>
    void start_read(CB broadcast_cb, shared_buffer buf, holder_ptr holder) {
        auto *ptr = buf.get();
        ba::async_read_until(
             m_sock
            ,ba::dynamic_buffer(*ptr)
            ,'\n'
            ,[this, buf=std::move(buf), broadcast_cb=std::move(broadcast_cb), holder=std::move(holder)]
             (const bs::error_code& ec, std::size_t size) mutable
            { on_readed(std::move(buf), std::move(broadcast_cb), ec, size, std::move(holder)); }
        );
    }
    template<typename CB>
    void on_readed(shared_buffer buf, CB broadcast_cb, const bs::error_code& ec, std::size_t rd, holder_ptr holder) {
        if ( ec ) {
            if ( ec == ba::error::eof ) {
                std::cerr
                    << "the client("
                    << m_peer.address().to_string() << ":" << m_peer.port()
                    << ") disconnected"
                    << std::endl;
            } else {
                std::cerr << "read error: " << ec.message() << std::endl;
            }

            return;
        }

        std::string_view sv{buf->data(), rd};
        auto pos = sv.find(' ');
        if ( pos == std::string_view::npos ) {
            std::cerr << "wrong string received: \"" << sv << "\"" << std::endl;

            start_read(std::move(broadcast_cb), std::move(buf), std::move(holder));

            return;
        } else {
            DEBUG_EXPR(std::cout << "on_readed: str=" << sv << std::flush;);
        }
        auto key = make_buffer(sv.data(), sv.data() + pos);
        auto val = make_buffer(sv.data() + pos + 1, sv.data() + sv.size());

        buf->erase(buf->begin(), buf->begin() + rd);

        m_state.update(
             std::move(key)
            ,std::move(val)
            ,[this, broadcast_cb, holder]
             (bool really, const shared_buffer &key, const shared_buffer &hash) mutable
             { on_updated(really, key, hash, std::move(broadcast_cb), std::move(holder)); }
        );

        start_read(std::move(broadcast_cb), std::move(buf), std::move(holder));
    }

private:
    // called from shared_state's strand
    template<typename CB>
    void on_updated(bool really, const shared_buffer &key, const shared_buffer &hash, CB broadcast_cb, holder_ptr /*holder*/) {
        // when `really` == true, it's mean that `shared_state` was really updated
        if ( really ) {
            auto msg = make_buffer(*key);
            *msg += ' ';
            (*msg).append(*hash);
            *msg += '\n';

            DEBUG_EXPR(std::cout << "broadcasting: " << *msg << std::flush;);

            broadcast_cb(std::move(msg));
        }
    }

private:
    template<typename CB>
    void send_impl(shared_buffer msg, CB cb, holder_ptr holder) {
        //std::cout << "send_impl(string): " << *msg << std::endl;

        auto *ptr = msg.get();
        ba::async_write(
             m_sock
            ,ba::buffer(*ptr)
            ,[this, msg=std::move(msg), holder=std::move(holder), cb=std::move(cb)]
             (const bs::error_code &ec, std::size_t)
             { sent(ec, std::move(cb)); }
        );
    }
    template<typename CB>
    void sent(const bs::error_code &ec, CB cb) {
        if ( ec ) {
            std::cerr << "write error: " << ec << std::endl;

            m_sock.close();

            return;
        }

        cb(ec);
    }

private:
    tcp::socket m_sock;
    tcp::endpoint m_peer;
    shared_state &m_state;
};

/**********************************************************************************************************************/

struct session_manager {
    session_manager(ba::io_context &ioc)
        :m_strand{ioc}
        ,m_list{}
    {}

    void add(std::weak_ptr<session> sptr) {
        ba::post(
             m_strand
            ,[this, sptr]
             ()
             { m_list.push_back(sptr); }
        );
    }

    void broadcast(shared_buffer msg) {
        ba::post(
             m_strand
            ,[this, msg=std::move(msg)](){
                for ( auto it = m_list.begin(); it != m_list.end(); ) {
                    if ( auto sptr = it->lock(); sptr ) {
                        ++it;
                        sptr->send(msg, [](bs::error_code){});
                    } else {
                        it = m_list.erase(it);
                    }
                }
            }
        );
    }

private:
    ba::io_context::strand m_strand;
    std::list<std::weak_ptr<session>> m_list;
};

/**********************************************************************************************************************/

struct acceptor {
    acceptor(ba::io_context &ioc, std::uint16_t port, session_manager &smgr, shared_state &state)
        :m_ioc{ioc}
        ,m_acc{ba::make_strand(m_ioc), tcp::endpoint{tcp::v4(), port}}
        ,m_smgr{smgr}
        ,m_state{state}
    {
        m_acc.set_option(ba::socket_base::reuse_address{true}); // can throw
        m_acc.set_option(tcp::no_delay{true}); // can throw
    }

    void start() {
        m_acc.async_accept(
             ba::make_strand(m_ioc)
            ,[this](const bs::error_code &ec, tcp::socket sock)
             { on_accepted(ec, std::move(sock)); }
        );
    }

private:
    void on_accepted(const bs::error_code &ec, tcp::socket sock) {
        if ( ec ) {
            std::cerr << "acceptor error: " << ec << std::endl;

            return;
        }

        const auto rep = sock.remote_endpoint();
        DEBUG_EXPR(std::cout << "new connection from: " << rep.address().to_string() << ":" << rep.port() << std::endl;);

        auto sptr = std::make_shared<session>(std::move(sock), m_state);

        sptr->start(
            [this]
            (shared_buffer msg)
            { m_smgr.broadcast(std::move(msg)); }
            ,sptr->shared_from_this()
        );

        m_smgr.add(sptr);

        start();
    }

private:
    ba::io_context &m_ioc;
    tcp::acceptor m_acc;
    session_manager &m_smgr;
    shared_state &m_state;
};

/**********************************************************************************************************************/

int main(int argc, char **argv) try {
    if ( argc != 3 ) {
        std::cout << "usage: " << argv[0] << " <PORT> <THREADS>(min 2)" << std::endl;

        return EXIT_FAILURE;
    }

    std::uint16_t port = std::atoi(argv[1]);
    int threads = std::atoi(argv[2]);
    threads -= 1;

    ba::io_context ioctx{threads};
    ioctx.post([]{ std::cout << "server started..." << std::endl; });

    shared_state state{ioctx};
    session_manager smgr{ioctx};
    acceptor acc{ioctx, port, smgr, state};
    acc.start();

    ba::signal_set signals{ioctx, SIGINT, SIGTERM};
    signals.async_wait([&ioctx](const bs::error_code &, int) {
        std::cout << "SIGINT/SIGTERM received!" << std::endl;

        ioctx.stop();
    });

    std::vector<std::thread> threadsv;
    threadsv.reserve(threads);
    for ( ; threads; --threads ) {
        threadsv.emplace_back([&ioctx]{ ioctx.run(); });
    }

    // we will blocked here until SIGINT/SIGTERM
    ioctx.run();

    // wait for all threads to exit
    for ( auto &it: threadsv ) {
        it.join();
    }

    std::cout << "server stopped!" << std::endl;

    return EXIT_SUCCESS;
} catch (const std::exception &ex) {
    std::cerr << "std::exception: " << ex.what() << std::endl;

    return EXIT_FAILURE;
}

/**********************************************************************************************************************/