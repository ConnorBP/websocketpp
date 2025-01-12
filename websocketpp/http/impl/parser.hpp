/*
 * Copyright (c) 2014, Peter Thorson. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the WebSocket++ Project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL PETER THORSON BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef HTTP_PARSER_IMPL_HPP
#define HTTP_PARSER_IMPL_HPP

#include <algorithm>
#include <cstdlib>
#include <istream>
#include <sstream>
#include <string>

namespace websocketpp {
namespace http {
namespace parser {

inline lib::error_code parser::set_version(std::string const & version) {
    // todo: validation?
    m_version = version;

    return lib::error_code();
}

inline std::string const & parser::get_header(std::string const & key) const {
    // This find is case insensitive due to the case insensitive comparator
    // templated into header_list.
    header_list::const_iterator h = m_headers.find(key);

    if (h == m_headers.end()) {
        return empty_header;
    } else {
        return h->second;
    }
}

inline bool parser::get_header_as_plist(std::string const & key,
    parameter_list & out) const
{
    header_list::const_iterator it = m_headers.find(key);

    if (it == m_headers.end() || it->second.size() == 0) {
        return false;
    }

    return this->parse_parameter_list(it->second,out);
}

inline lib::error_code parser::append_header(std::string const & key, std::string const &
    val)
{
    if (std::find_if(key.begin(),key.end(),is_not_token_char) != key.end()) {
        return error::make_error_code(error::invalid_header_name);
    }

    if (this->get_header(key).empty()) {
        m_headers[key] = val;
    } else {
        m_headers[key] += ", " + val;
    }
    return lib::error_code();
}

inline lib::error_code parser::replace_header(std::string const & key, std::string const &
    val)
{
    if (std::find_if(key.begin(),key.end(),is_not_token_char) != key.end()) {
        return error::make_error_code(error::invalid_header_name);
    }

    m_headers[key] = val;
    return lib::error_code();
}

inline lib::error_code parser::remove_header(std::string const & key)
{
    if (std::find_if(key.begin(),key.end(),is_not_token_char) != key.end()) {
        return error::make_error_code(error::invalid_header_name);
    }

    m_headers.erase(key);
    return lib::error_code();
}

inline lib::error_code parser::set_body(std::string value) {
    lib::error_code ec;
    if (value.size() == 0) {
        ec = remove_header(Header_ContentLength);
        if (ec) { return ec; }

        m_body.clear();
        return lib::error_code();
    }

    if (m_body_bytes_max && value.size() > m_body_bytes_max) {
        return error::make_error_code(error::body_too_large);
    }

    std::stringstream len;
    len << value.size();
    ec = replace_header(Header_ContentLength, len.str());
    if (ec) { return ec; }

    m_body = std::move(value);
    return lib::error_code();
}

inline void parser::consume_body()
{
	m_body.clear();
}

inline bool parser::parse_parameter_list(std::string const & in,
    parameter_list & out) const
{
    if (in.size() == 0) {
        return false;
    }

    std::string::const_iterator it;
    it = extract_parameters(in.begin(),in.end(),out);
    return (it == in.begin());
}

inline bool parser::prepare_body(lib::error_code & ec) {
	ec.clear();

	parameter_list ce_list;
	if (!get_header_as_plist(Header_ContentEncoding, ce_list)) {
		for (const auto& param : ce_list) {
			auto encoding = content_encoding::from_string(param.first);
			if (encoding)
			{
				m_content_encoding.push_back(*encoding);
			}
			else {
				ec = error::make_error_code(error::unknown_content_encoding);
				return false;
			}
		}

		if (m_content_encoding.size() > 3)
		{
			ec = error::make_error_code(error::unsupported_content_encoding);
			return false;
		}
	}

	parameter_list te_list;
	if (!get_header_as_plist(Header_TransferEncoding, te_list) && !te_list.empty()) {
		for (const auto& param : te_list) {
			if (param.first == "gzip" || param.first == "x-gzip")
				m_transfer_encoding.push_back(transfer_encoding::gzip);
			else if (param.first == "compress")
				m_transfer_encoding.push_back(transfer_encoding::compress);
			else if (param.first == "deflate")
				m_transfer_encoding.push_back(transfer_encoding::deflate);
			else if (param.first == "chunked")
				m_transfer_encoding.push_back(transfer_encoding::chunked);
			else {
				ec = error::make_error_code(error::unknown_transfer_encoding);
				return false;
			}
		}

		if (m_transfer_encoding.size() > 3)
		{
			ec = error::make_error_code(error::unsupported_transfer_encoding);
			return false;
		}

		if (std::find(m_transfer_encoding.begin(), m_transfer_encoding.end(), transfer_encoding::chunked) != m_transfer_encoding.end())
			return true; // no Content-Length for chunked encoding!
	}

	const std::string cl_header = get_header(Header_ContentLength);
    if (!cl_header.empty()) {
        char * end;

        // TODO: not 100% sure what the compatibility of this method is. Also,
        // I believe this will only work up to 32bit sizes. Is there a need for
        // > 4GiB HTTP payloads?
        m_body_bytes_total = m_body_bytes_needed = std::strtoul(cl_header.c_str(),&end,10);
		if (end != &*cl_header.cend()) {
			ec = error::make_error_code(error::invalid_format);
            return false;
		}

        if (m_body_bytes_max && m_body_bytes_total > m_body_bytes_max) {
            ec = error::make_error_code(error::body_too_large);
            return false;
        }

        return m_body_bytes_needed;
    } else {
        return false;
    }
}

inline size_t parser::process_body(char const * buf, size_t len,
    lib::error_code & ec)
{
	if (!len)
		return 0;

    if (std::find(m_transfer_encoding.begin(), m_transfer_encoding.end(), transfer_encoding::chunked) != m_transfer_encoding.end()) {
        // for chunked encoding, read chunks of the body
		if (m_body_bytes_needed) { // reading previously started chunk, same as plain encoding!
			const size_t processed = std::min(m_body_bytes_needed, len);
			m_body.append(buf, processed);
			m_body_bytes_needed -= processed;
			ec = lib::error_code();
			return processed;
		} else { // new chunk
			// sizes of chunks which are given by the first byte of the response body
			const char* newline = std::search(buf, buf + len, http_crlf, http_crlf + sizeof(http_crlf) - 1);
			if (newline == buf + len)
			{
				ec = error::make_error_code(error::invalid_format);
				return 0;
			}

			const std::string chunkSizeHex(buf, newline);
			char * end;
			m_body_bytes_needed = std::strtoul(chunkSizeHex.c_str(),&end,16);
			m_body_bytes_total += m_body_bytes_needed;
			if (end != &*chunkSizeHex.cend()) {
				ec = error::make_error_code(error::invalid_format);
				return 0;
			}

			if (m_body_bytes_max && m_body_bytes_total > m_body_bytes_max) {
				ec = error::make_error_code(error::body_too_large);
				return 0;
			}

			if (m_body_bytes_needed == 0) { // this is how the last chunk is marked
				return len; // pretend we handled everything!
			}

			const size_t processed = (newline - buf) + sizeof(http_crlf) - 1;
			return processed + process_body(buf + processed, len - processed, ec);
		}
    } else {
		const size_t processed = std::min(m_body_bytes_needed, len);
		m_body.append(buf, processed);
		m_body_bytes_needed -= processed;
		ec = lib::error_code();
		return processed;
    }
}

inline lib::error_code parser::process_header(std::string::iterator begin,
    std::string::iterator end)
{
    std::string::iterator cursor = std::search(
        begin,
        end,
        header_separator,
        header_separator + sizeof(header_separator) - 1
    );

    if (cursor == end) {
        return error::make_error_code(error::body_too_large);
    }

    // any error from append header represents our final error status
    return append_header(
        strip_lws(std::string(begin,cursor)),
        strip_lws(std::string(cursor+sizeof(header_separator)-1,end)));
}

inline header_list const & parser::get_headers() const {
    return m_headers;
}

inline std::string parser::raw_headers() const {
    std::stringstream raw;

    header_list::const_iterator it;
    for (it = m_headers.begin(); it != m_headers.end(); it++) {
        raw << it->first << ": " << it->second << "\r\n";
    }

    return raw.str();
}



} // namespace parser
} // namespace http
} // namespace websocketpp

#endif // HTTP_PARSER_IMPL_HPP
