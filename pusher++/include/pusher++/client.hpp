//          Copyright Ben Pope 2017.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef PUSHERPP_CLIENT_HPP
#define PUSHERPP_CLIENT_HPP

#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/handler_type.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <string>
#include <map>

#include "event.hpp"
#include "channel_proxy.hpp"
#include "detail/client/read.hpp"
#include "detail/client/signal_filter.hpp"
#include "detail/client/write.hpp"

namespace pusher
{
    template<typename SocketT>
    class client
    {
    public:
        using signal_filter = detail::client::signal_filter<std::string(*)(event const&)>;
        boost::beast::websocket::stream<SocketT> socket_;
        //boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket&>> socket_;
        boost::asio::ip::tcp::resolver resolver_;
        std::string host_;
        std::string handshake_resource_;
        std::string auth_end_point_;
        boost::beast::flat_buffer read_buf_;
        detail::client::signal events_;
        signal_filter filtered_channels_;
        std::map<std::string, signal_filter> channels_;
        signal_filter filtered_events_;

    public:
        client(boost::asio::io_service& ios, std::string key, std::string host)
          : socket_{ios}
          , resolver_{ios}
//          , host_{"ws-" + std::move(cluster) + ".pusher.com"}
          //, host_{"realtime-pusher.ably.io"}
          , host_{host}
          , handshake_resource_{"/app/" + std::move(key) + "?client=pusher++&version=0.01&protocol=7"}
          , events_{}
          , filtered_channels_{detail::client::filtered_signal(&detail::client::by_channel)}
          , filtered_events_{detail::client::filtered_signal(&detail::client::by_name)}
        {}

        client(boost::asio::io_service& ios, boost::asio::ssl::context& ctx, std::string key, std::string token, std::string host)
          : socket_{ios, ctx}
          , resolver_{ios}
          //, host_{"ws-" + std::move(cluster) + ".pusher.com"}
          //, host_{"realtime-pusher.ably.io"}
          , host_{host}
          , handshake_resource_{"/app/" + std::move(key) + "?client=pusher++&version=0.01&protocol=7"}
          , auth_end_point_{"/pusher/auth/?token=" + std::move(token)}
          , events_{}
          , filtered_channels_{detail::client::filtered_signal(&detail::client::by_channel)}
          , filtered_events_{detail::client::filtered_signal(&detail::client::by_name)}
        {}

        ~client()
        {
            try
            {
                disconnect();
            }
            catch(std::exception& except)
            {
                std::cout << "Caught exception - " << except.what() << std::endl;
            }

        }


        void initialise()
        {
            filtered_channels_.connect_source(events_);
            filtered_events_.connect_source(events_);
        }

        template<typename TokenT>
        auto async_connect(TokenT&& token)
        {
            initialise();

            typename boost::asio::handler_type<TokenT, void(boost::system::error_code)>::type handler(std::forward<TokenT>(token));
            boost::asio::async_result<decltype(handler)> result(handler);
            using query = boost::asio::ip::tcp::resolver::query;
            resolver_.async_resolve(query{host_, "http"}, [this, handler](auto ec, auto endpoint) mutable
            {
                if(ec)
                    return handler(ec);

                boost::asio::async_connect(socket_.lowest_layer(), endpoint, [this, handler](auto ec, auto) mutable
                {
                    if(ec)
                        return handler(ec);

                    socket_.async_handshake(host_, handshake_resource_, [this, handler](auto ec) mutable
                    {
                        if(ec)
                            return handler(ec);

                        this->read_impl();
                        return handler(ec);
                    });
                });
            });
            return result.get();
        }

        auto connect()
        {
            initialise();

            std::cout << "BEFORE CONNECT: Is open  - " << socket_.is_open() << std::endl;
            //boost::asio::connect(socket_.next_layer(), resolver_.resolve(boost::asio::ip::tcp::resolver::query{host_, "80"}));
            //boost::asio::connect(socket_.lowest_layer(), resolver_.resolve(boost::asio::ip::tcp::resolver::query{host_, "443"}));
            boost::asio::connect(socket_.lowest_layer(), resolver_.resolve(boost::asio::ip::tcp::resolver::query{host_, "443"}));
            socket_.next_layer().handshake(boost::asio::ssl::stream_base::client);
            socket_.handshake(host_, handshake_resource_);
            std::cout << "AFTER CONNECT: Is open  - " << socket_.is_open() << std::endl;

            read_impl();
        }

        void disconnect()
        {
            resolver_.cancel();
            socket_.close(boost::beast::websocket::close_code::normal);
        }

        auto channel(std::string const& name, std::string const& token = "", std::string const& channelData = "")
        {
            auto channel_result = filtered_channels_.filtered_.emplace(name, detail::client::signal{});
            auto& channel = channel_result.first->second;
            bool inserted = channel_result.second;

            auto result = channels_.emplace(name, detail::client::filtered_signal(&detail::client::by_name));
            result.first->second.connect_source(channel);

            if(inserted)
                subscribe(name, token, channelData);

            return channel_proxy(&(result.first->second));
        }

        template<typename FuncT>
        auto bind_all(FuncT&& func)
        {
            return filtered_events_.connect(std::forward<FuncT>(func));
        }

        template<typename FuncT>
        auto bind(std::string const& event_name, FuncT&& func)
        {
            return filtered_events_.connect(event_name, std::forward<FuncT>(func));
        }

    private:
        auto subscribe(std::string const& channel, std::string const& token = "", std::string const& channelData = "")
        {
            socket_.write(boost::asio::buffer(detail::client::make_subscription(channel, token, channelData)));
        }

        void read_impl()
        {
            return socket_.async_read(read_buf_, [this](auto ec, std::size_t bytes_written)
            {
                if(ec)
                    return;

                events_(detail::client::make_event(read_buf_));
                read_buf_.consume(read_buf_.size());

                this->read_impl();
            });
        }
    };
}

#endif // PUSHERPP_CLIENT_HPP
