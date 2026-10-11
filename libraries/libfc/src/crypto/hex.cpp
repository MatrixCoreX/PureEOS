#include <fc/crypto/hex.hpp>
#include <fc/exception/exception.hpp>
#include <stdexcept>

namespace fc {

    uint8_t from_hex( char c ) {
      if( c >= '0' && c <= '9' )
        return c - '0';
      if( c >= 'a' && c <= 'f' )
          return c - 'a' + 10;
      if( c >= 'A' && c <= 'F' )
          return c - 'A' + 10;
      FC_THROW_EXCEPTION( exception, "Invalid hex character '${c}'", ("c", std::string(&c,1) ) );
      return 0;
    }

    std::string to_hex( const char* d, uint32_t s ) 
    {
        std::string r;
        // ABI decoding falls back to hex even after its deadline has expired.
        // Allocate once and fill directly: per-character append bookkeeping is
        // costly for large signatures, especially in unoptimized builds.
        if( static_cast<size_t>(s) > r.max_size() / 2 )
            throw std::length_error("hex string exceeds maximum size");
        r.resize(static_cast<size_t>(s) * 2);
        constexpr char digits[] = "0123456789abcdef";
        const auto* input = reinterpret_cast<const unsigned char*>(d);
        auto* output = r.data();
        for( size_t i = 0; i < s; ++i ) {
            output[2 * i] = digits[input[i] >> 4];
            output[2 * i + 1] = digits[input[i] & 0x0f];
        }
        return r;
    }

    size_t from_hex( const std::string& hex_str, char* out_data, size_t out_data_len ) {
        std::string::const_iterator i = hex_str.begin();
        uint8_t* out_pos = (uint8_t*)out_data;
        uint8_t* out_end = out_pos + out_data_len;
        while( i != hex_str.end() && out_end != out_pos ) {
          *out_pos = from_hex( *i ) << 4;   
          ++i;
          if( i != hex_str.end() )  {
              *out_pos |= from_hex( *i );
              ++i;
          }
          ++out_pos;
        }
        return out_pos - (uint8_t*)out_data;
    }
    std::string to_hex( const std::vector<char>& data )
    {
       if( data.size() )
          return to_hex( data.data(), data.size() );
       return "";
    }

}
