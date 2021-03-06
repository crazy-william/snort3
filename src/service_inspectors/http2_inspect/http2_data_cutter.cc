//--------------------------------------------------------------------------
// Copyright (C) 2020-2020 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_data_cutter.cc author Maya Dagon <mdagon@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_data_cutter.h"

#include "service_inspectors/http_inspect/http_flow_data.h"
#include "service_inspectors/http_inspect/http_stream_splitter.h"

#include "http2_dummy_packet.h"

using namespace snort;
using namespace HttpCommon;
using namespace Http2Enums;

static std::string create_chunk_hdr(uint32_t len)
{
    std::stringstream stream;
    stream<<std::hex<< len;
    return stream.str() + "\r\n";
}

Http2DataCutter::Http2DataCutter(Http2FlowData* _session_data, uint32_t len,
    HttpCommon::SourceId src_id, bool is_padded) :
    session_data(_session_data),  source_id(src_id), frame_length(len), data_len(len)
{
    data_state = (is_padded) ? PADDING_LENGTH : DATA;
}

// Scan data frame, extract information needed for http scan.
// http scan will need the data only, stripped of padding and header.
bool Http2DataCutter::http2_scan(const uint8_t* data, uint32_t length,
    uint32_t* flush_offset)
{
    *flush_offset = cur_data_offset = cur_data = cur_padding = 0;

    if (frame_bytes_seen == 0)
    {
        frame_bytes_seen = cur_data_offset = FRAME_HEADER_LENGTH;
        length -= FRAME_HEADER_LENGTH;
        *flush_offset = FRAME_HEADER_LENGTH;
    }

    uint32_t cur_pos = 0;

    while ((cur_pos < length) && (data_state != FULL_FRAME))
    {
        switch (data_state)
        {
        case PADDING_LENGTH:
            padding_len = *(data + cur_data_offset);

            if (data_len <= padding_len)
            {
                *session_data->infractions[source_id] += INF_PADDING_LEN;
                session_data->events[source_id]->create_event(EVENT_PADDING_LEN);
                return false;
            }
            // FIXIT temporary - till multiple data frames sent to http
            if (data_len == (padding_len + 1))
                return false;
            data_len -= (padding_len + 1);
            data_state = DATA;
            cur_pos++;
            cur_data_offset++;
            break;
        case DATA:
          {
            const uint32_t missing = data_len - data_bytes_read;
            cur_data = ((length - cur_pos) >= missing) ?
                missing : (length - cur_pos);
            data_bytes_read += cur_data;
            cur_pos += cur_data;
            if (data_bytes_read == data_len)
                data_state = padding_len ? PADDING : FULL_FRAME;
            break;
          }
        case PADDING:
          {
            const uint32_t missing = padding_len - padding_read;
            cur_padding = ((length - cur_pos) >= missing) ?
                missing : (length - cur_pos);
            cur_pos += cur_padding;
            padding_read += cur_padding;
            if (padding_read == padding_len)
                data_state = FULL_FRAME;
            break;
          }
        default:
            break;
        }
    }

    frame_bytes_seen += cur_pos;
    session_data->scan_remaining_frame_octets[source_id] = frame_length - frame_bytes_seen;
    *flush_offset += cur_pos;

    return true;
}

// Call http scan. Wrap data with chunk header and end of chunk.
StreamSplitter::Status Http2DataCutter::http_scan(const uint8_t* data, uint32_t* flush_offset)
{
    StreamSplitter::Status scan_result = StreamSplitter::SEARCH;
    uint32_t http_flush_offset = 0;
    Http2DummyPacket dummy_pkt;
    dummy_pkt.flow = session_data->flow;
    uint32_t unused = 0;

    // first phase supports only flush of full packet
    switch (http_state)
    {
    case NONE_SENT:
      {
        if (cur_data)
        {
            std::string chunk_hdr = create_chunk_hdr(data_len);
            scan_result = session_data->hi_ss[source_id]->scan(&dummy_pkt,
                (const unsigned char*)chunk_hdr.c_str(),
                chunk_hdr.length(), unused, &http_flush_offset);
            bytes_sent_http += chunk_hdr.length();
            http_state = HEADER_SENT;
            if (scan_result != StreamSplitter::SEARCH)
                return StreamSplitter::ABORT;
        }
      }   // fallthrough
    case HEADER_SENT:
      {
        if (cur_data)
        {
            scan_result = session_data->hi_ss[source_id]->scan(&dummy_pkt, data + cur_data_offset,
                cur_data, unused, &http_flush_offset);
            bytes_sent_http += cur_data;

            if (scan_result != StreamSplitter::SEARCH)
                return StreamSplitter::ABORT;
        }
        if (data_state == FULL_FRAME)
        {
            scan_result = session_data->hi_ss[source_id]->scan(&dummy_pkt, (const unsigned
                char*)"\r\n0\r\n",
                5, unused, &http_flush_offset);
            bytes_sent_http +=5;
            assert(scan_result == StreamSplitter::FLUSH);

            session_data->scan_octets_seen[source_id] = 0;
            session_data->scan_remaining_frame_octets[source_id] = 0;
        }
      }
    }

    if (scan_result != StreamSplitter::FLUSH)
        *flush_offset = 0;

    return scan_result;
}

StreamSplitter::Status Http2DataCutter::scan(const uint8_t* data, uint32_t length,
    uint32_t* flush_offset)
{
    if (!http2_scan(data, length, flush_offset))
        return StreamSplitter::ABORT;

    return Http2DataCutter::http_scan(data, flush_offset);
}

const StreamBuffer Http2DataCutter::reassemble(unsigned total, unsigned offset, const
    uint8_t* data, unsigned len)
{
    StreamBuffer frame_buf { nullptr, 0 };

    if (offset == 0)
    {
        padding_read = data_bytes_read = hdr_bytes_read = 0;
    }
    cur_data = cur_padding = cur_data_offset = 0;

    unsigned cur_pos = 0;
    while (cur_pos < len)
    {
        switch (reassemble_state)
        {
        case SKIP_FRAME_HDR:
          {
            if (hdr_bytes_read == 0)
            {
                session_data->frame_header[source_id] = new uint8_t[FRAME_HEADER_LENGTH];
                session_data->frame_header_size[source_id] = FRAME_HEADER_LENGTH;
            }
            const uint32_t missing = FRAME_HEADER_LENGTH - hdr_bytes_read;
            const uint32_t cur_frame = ((len - cur_pos) < missing) ? (len - cur_pos) : missing;
            memcpy(session_data->frame_header[source_id] + hdr_bytes_read, data + cur_pos,
                cur_frame);
            hdr_bytes_read += cur_frame;
            cur_pos += cur_frame;
            if (hdr_bytes_read == FRAME_HEADER_LENGTH)
            {
                cur_data_offset = cur_pos;
                reassemble_state = (padding_len) ? SKIP_PADDING_LEN : SEND_CHUNK_HDR;
            }

            break;
          }
        case SKIP_PADDING_LEN:
            cur_pos++;
            cur_data_offset++;
            reassemble_state = SEND_CHUNK_HDR;
            break;
        case SEND_CHUNK_HDR:
          {
            std::string chunk_hdr = create_chunk_hdr(data_len);
            unsigned copied;
            session_data->hi_ss[source_id]->reassemble(session_data->flow,
                bytes_sent_http, 0, (const uint8_t*)chunk_hdr.c_str(), chunk_hdr.length(), 0,
                copied);
            assert(copied == (unsigned)chunk_hdr.length());
            reassemble_state = SEND_DATA;
          }   // fallthrough
        case SEND_DATA:
          {
            const uint32_t missing = data_len - data_bytes_read;
            cur_data = ((len - cur_pos) >= missing) ? missing : (len - cur_pos);
            data_bytes_read += cur_data;
            cur_pos += cur_data;

            unsigned copied;
            frame_buf = session_data->hi_ss[source_id]->reassemble(session_data->flow,
                bytes_sent_http, 0, data + cur_data_offset, cur_data,
                0, copied);
            assert(copied == (unsigned)cur_data);

            if (data_bytes_read == data_len)
                reassemble_state = (padding_len) ? SKIP_PADDING : SEND_CRLF;

            break;
          }
        case SKIP_PADDING:
          {
            const uint32_t missing = padding_len - padding_read;
            cur_padding = ((len - cur_pos) >= missing) ?
                missing : (len - cur_pos);
            cur_pos += cur_padding;
            padding_read += cur_padding;
            if (padding_read == padding_len)
                reassemble_state = SEND_CRLF;
            break;
          }

        default:
            break;
        }
    }

    if (len + offset == total)
        assert(reassemble_state == SEND_CRLF);

    if (reassemble_state == SEND_CRLF)
    {
        unsigned copied;
        frame_buf = session_data->hi_ss[source_id]->reassemble(session_data->flow,
            bytes_sent_http, 0,(const unsigned char*)"\r\n0\r\n", 5, PKT_PDU_TAIL, copied);
        assert(copied == 5);

        assert(frame_buf.data != nullptr);
        session_data->frame_data[source_id] = const_cast <uint8_t*>(frame_buf.data);
        session_data->frame_data_size[source_id] = frame_buf.length;
    }

    return frame_buf;
}

