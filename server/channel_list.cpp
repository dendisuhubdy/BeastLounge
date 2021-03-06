//
// Copyright (c) 2018-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/BeastLounge
//

#include "channel.hpp"
#include "channel_list.hpp"
#include "message.hpp"
#include "rpc.hpp"
#include "server.hpp"
#include "service.hpp"
#include "user.hpp"
#include <boost/beast/_experimental/json/parser.hpp>
#include <boost/container/flat_set.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/shared_lock_guard.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <boost/smart_ptr/make_unique.hpp>
#include <limits>
#include <mutex>
#include <vector>

extern
void
make_room(
    channel_list& list,
    beast::string_view name);

namespace {

class channel_list_impl
    : public channel_list
    , public service
{
    struct element
    {
        boost::shared_ptr<channel> c;
        std::size_t next = 0;
    };

    using mutex = boost::shared_mutex;
    using lock_guard = boost::lock_guard<mutex>;
    using shared_lock_guard = boost::shared_lock_guard<mutex>;

    server& srv_;
    mutex mutable m_;
    std::vector<element> v_;
    boost::container::flat_set<channel*> users_;
    std::atomic<uid_type> next_uid_;
    std::atomic<std::size_t> next_cid_;

public:
    channel_list_impl(
        server& srv)
        : srv_(srv)
        , next_uid_(1000)
        , next_cid_(1000)
    {
        // element 0 is unused
        v_.resize(1);

        make_room(*this, "General");
    }

    //--------------------------------------------------------------------------
    //
    // service
    //
    //--------------------------------------------------------------------------

    void
    on_start() override
    {
    }

    void
    on_stop() override
    {
    }

    //--------------------------------------------------------------------------
    //
    // channel_list
    //
    //--------------------------------------------------------------------------

    boost::shared_ptr<channel>
    at(std::size_t cid) const override
    {
        shared_lock_guard lock(m_);
        if(cid >= v_.size())
            return nullptr;
        return v_[cid].c;
    }

    void
    dispatch(
        net::const_buffer b,
        user& u) override
    {
        rpc_request req;
        try
        {
            json::parser pr;
            beast::error_code ec;

            // Parse the buffer into JSON
            pr.write(b, ec);
            if(ec)
                throw rpc_error(
                    rpc_code::parse_error,
                    ec.message());

            // Validate and extract the JSON-RPC request
            req.extract(pr.release(), ec);
            if(ec)
                throw rpc_error(
                    rpc_code::invalid_request,
                    ec.message());

            // Validate and extract the channel id
            auto& jv_cid = checked_number(req.params, "cid");
            if(! jv_cid.is_uint64())
                throw rpc_error(
                    rpc_code::invalid_params,
                    "Invalid cid");
            if(jv_cid.get_uint64() >
                (std::numeric_limits<std::size_t>::max)())
                throw rpc_error(
                    rpc_code::invalid_params,
                    "Invalid cid");
            auto const cid = static_cast<std::size_t>(
                jv_cid.get_uint64());

            // Lookup cid
            auto c = at(cid);
            if(! c)
                throw rpc_error(
                    rpc_code::invalid_params,
                    "Unknown cid");

            // Prepare the response
            json::value res = json::object{};
            if(req.id)
                res.emplace("id", *req.id);
            auto& result =
                res.emplace("result", nullptr).first->second;

            // Dispatch the request
            c->dispatch(result, req, u);

            // Send the response if `id` is set
            if(req.id)
                u.send(make_message(res));
        }
        catch(rpc_error const& e)
        {
            // Send the error if `id` is set
            if(req.id)
                u.send(make_message(e.to_json(req.id)));
        }
    }

    uid_type
    next_uid() noexcept override
    {
        return ++next_uid_;
    }

    std::size_t
    next_cid() noexcept override
    {
        return ++next_cid_;
    }

    void
    insert(boost::shared_ptr<channel> c) override
    {
        auto const cid = c->cid();
        lock_guard lock(m_);
        v_.resize(std::max<std::size_t>(
            cid + 1, v_.size()));
        BOOST_ASSERT(v_[cid].c == nullptr);
        v_[cid].c = std::move(c);
    }

    void
    erase(channel const& c) override
    {
        auto const cid = c.cid();
        lock_guard lock(m_);
        BOOST_ASSERT(cid < v_.size());
        v_[cid].c = nullptr;
    }

    //--------------------------------------------------------------------------
    //
    // channel_list_impl
    //
    //--------------------------------------------------------------------------
};

} // (anon)

std::unique_ptr<channel_list>
make_channel_list(server& srv)
{
    return boost::make_unique<
        channel_list_impl>(srv);
}
