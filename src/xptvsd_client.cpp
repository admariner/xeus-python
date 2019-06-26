/***************************************************************************
* Copyright (c) 2018, Martin Renou, Johan Mabille, Sylvain Corlay and      *
* Wolf Vollprecht                                                          *
*                                                                          *
* Distributed under the terms of the BSD 3-Clause License.                 *
*                                                                          *
* The full license is in the file LICENSE, distributed with this software. *
****************************************************************************/

#include "nlohmann/json.hpp"

#include "xptvsd_client.hpp"

namespace nl = nlohmann;

namespace xpyt
{
    xptvsd_client::xptvsd_client(zmq::context_t& context,
                                 int socket_linger)
        : m_ptvsd_socket(context, zmq::socket_type::stream)
        , m_id_size(256)
        , m_publisher(context, zmq::socket_type::pub)
        , m_controller(context, zmq::socket_type::router)
        , m_request_stop(false)
    {
        m_ptvsd_socket.setsockopt(ZMQ_LINGER, socket_linger);
        m_publisher.setsockopt(ZMQ_LINGER, socket_linger);
        m_controller.setsockopt(ZMQ_LINGER, socket_linger);
    }

    void xptvsd_client::start_debugger(std::string ptvsd_end_point,
                                       std::string publisher_end_point,
                                       std::string controller_end_point)
    {
        m_publisher.connect(publisher_end_point);
        m_controller.connect(controller_end_point);
        
        m_ptvsd_socket.connect(ptvsd_end_point);
        m_ptvsd_socket.getsockopt(ZMQ_IDENTITY, m_socket_id, &m_id_size);

        // Tells the controller that the connection with
        // ptvsd has been established
        m_controller.send(zmq::message_t("ACK", 3));
        
        zmq::pollitem_t items[] = {
            { m_ptvsd_socket, 0, ZMQ_POLLIN, 0 },
            { m_controller, 0, ZMQ_POLLIN, 0 }
        };
        
        while(!m_request_stop)
        {
            zmq::poll(&items[0], 2, -1);

            if(items[0].revents & ZMQ_POLLIN)
            {
                handle_ptvsd_socket();
            }

            process_message_queue();

            if(items[1].revents & ZMQ_POLLIN)
            {
                handle_control_socket();
            }
        }

        m_ptvsd_socket.disconnect(ptvsd_end_point);
        m_controller.disconnect(controller_end_point);
        m_publisher.disconnect(publisher_end_point);
    }

    void xptvsd_client::process_message_queue()
    {
        while(!m_message_queue.empty())
        {
            const std::string& raw_message = m_message_queue.back();
            nl::json message = nl::json::parse(raw_message);
            // message is either an event or a response
            // TODO: handle the message
            m_message_queue.pop();
        }
    }

    void xptvsd_client::handle_ptvsd_socket()
    {
        using size_type = std::string::size_type;
        
        std::string buffer = "";
        bool messages_received = false;
        size_type header_pos = std::string::npos;
        size_type separator_pos = std::string::npos;
        size_type msg_size = 0;
        size_type msg_pos = std::string::npos;
        size_type hint = 0;

        while(!messages_received)
        {
            while(header_pos == std::string::npos)
            {
                append_tcp_message(buffer);
                header_pos = buffer.find(HEADER, hint);
            }

            separator_pos = buffer.find(SEPARATOR, header_pos + HEADER_LENGTH);
            while(separator_pos == std::string::npos)
            {
                hint = buffer.size();
                append_tcp_message(buffer);
                separator_pos = buffer.find(SEPARATOR, hint);
            }

            msg_size = std::stoull(buffer.substr(header_pos + HEADER_LENGTH, separator_pos));
            msg_pos = separator_pos + SEPARATOR_LENGTH;

            // The end of the buffer does not contain a full message
            while(buffer.size() - msg_pos < msg_size)
            {
                append_tcp_message(buffer);
            }

            // The end of the buffer contains a full message
            if(buffer.size() - msg_pos == msg_size)
            {
                m_message_queue.push(buffer.substr(msg_pos));
                messages_received = true;
            }
            else
            {
                // The end of the buffer contains a full message
                // and the beginning of a new one. We push the first
                // one in the queue, and loop again to get the next
                // one.
                hint = msg_pos + msg_size;
                m_message_queue.push(buffer.substr(msg_pos, hint));
                header_pos = std::string::npos;
                separator_pos = std::string::npos;
            }
        }
    }

    void xptvsd_client::handle_control_socket()
    {
        zmq::message_t message;
        m_controller.recv(&message);

        // Sends a ZMQ header (required for stream socket) and forwards
        // the message
        m_ptvsd_socket.send(zmq::message_t(m_socket_id, m_id_size));
        m_ptvsd_socket.send(message);
    }

    void xptvsd_client::append_tcp_message(std::string& buffer)
    {
        // First message is a ZMQ header that we discard
        zmq::message_t header;
        m_ptvsd_socket.recv(&header);

        zmq::message_t content;
        m_ptvsd_socket.recv(&content);

        buffer += std::string(content.data<const char>(), content.size());
    }
}

