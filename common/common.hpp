
#ifndef __shared_state_server__common_hpp__included
#define __shared_state_server__common_hpp__included

#include <array>
#include <string>
#include <memory>
#include <chrono>
#include <cstring>

/**********************************************************************************************************************/
// holder

struct session;
using holder_ptr = std::shared_ptr<session>;

/**********************************************************************************************************************/
// shared buffer

using shared_buffer = std::shared_ptr<std::string>;

shared_buffer make_buffer(std::string str = {}) {
    return std::make_shared<shared_buffer::element_type>(std::move(str));
}

shared_buffer make_buffer(const char *beg, const char *end) {
    return make_buffer(std::string{beg, end});
}

/**********************************************************************************************************************/
// fnv1a

constexpr std::uint32_t fnv1a(std::string_view str) {
    std::uint32_t seed = 0x811c9dc5;
    for ( const auto *it = str.begin(); it != str.end(); ++it ) {
        seed = (std::uint32_t)((seed ^ ((std::uint32_t)*it)) * ((std::uint64_t)0x01000193));
    }

    return seed;
}

/**********************************************************************************************************************/

inline std::uint64_t ms_time() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/**********************************************************************************************************************/

template<std::size_t N>
struct average {
    average()
        :m_arr{{}}
    {}

    void update(std::size_t i) {
        std::memcpy(m_arr.begin(), m_arr.begin() + 1, sizeof(std::size_t) * N-1);
        m_arr.back() = i;
    }

    std::size_t avg() const {
        std::size_t res = 0;
        for ( const auto &it: m_arr ) {
            res += it;
        }

        return res / N;
    }
private:
    std::array<std::size_t, N> m_arr;
};

/**********************************************************************************************************************/

// PING
// SYNC
// DATA

template<typename PingCB, typename SyncCB, typename DataCB>
bool handle_incomming(shared_buffer str, PingCB ping_cb, SyncCB sync_cb, DataCB data_cb, holder_ptr holder) {
    constexpr auto ping_hash = fnv1a("PING");
    constexpr auto sync_hash = fnv1a("SYNC");
    constexpr auto data_hash = fnv1a("DATA");

    if ( str->length() > 4 && *(str->data() + 4) == ' ' ) {
        auto key = std::string_view{str->data(), 4};
        auto hash = fnv1a(key);
        switch ( hash ) {
            case ping_hash: {
                ping_cb(std::move(str), std::move(holder));
                return true;
            }
            case sync_hash: {
                sync_cb(std::move(str), std::move(holder));
                return true;
            }
            case data_hash: {
                data_cb(std::move(str), std::move(holder));
                return true;
            }
            default: return false;
        }
    }

    return false;
}

/**********************************************************************************************************************/

#endif // __shared_state_server__common_hpp__included
