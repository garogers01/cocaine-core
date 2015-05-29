#include "cocaine/detail/service/node/overseer.hpp"

#include <blackhole/scoped_attributes.hpp>

#include "cocaine/context.hpp"

#include "cocaine/detail/service/node/manifest.hpp"
#include "cocaine/detail/service/node/profile.hpp"

#include "cocaine/detail/service/node/balancing/base.hpp"
#include "cocaine/detail/service/node/balancing/null.hpp"
#include "cocaine/detail/service/node/dispatch/client.hpp"
#include "cocaine/detail/service/node/dispatch/handshaker.hpp"
#include "cocaine/detail/service/node/dispatch/worker.hpp"
#include "cocaine/detail/service/node/slave/control.hpp"
#include "cocaine/detail/service/node/slot.hpp"

namespace ph = std::placeholders;

using namespace cocaine;

struct collector_t {
    std::size_t active;

    explicit
    collector_t(const overseer_t::pool_type& pool):
        active{}
    {
        for (const auto& it : pool) {
            const auto load = it.second.load();
            if (it.second.active() && load) {
                active++;
            }
        }
    }
};

overseer_t::overseer_t(context_t& context,
                       manifest_t manifest,
                       profile_t profile,
                       std::shared_ptr<asio::io_service> loop):
    log(context.log(format("%s/overseer", manifest.name))),
    context(context),
    birthstamp(std::chrono::high_resolution_clock::now()),
    manifest(std::move(manifest)),
    profile_(profile),
    loop(loop),
    stats{}
{
    COCAINE_LOG_TRACE(log, "overseer has been initialized");
}

overseer_t::~overseer_t() {
    COCAINE_LOG_TRACE(log, "overseer has been destroyed");
}

profile_t
overseer_t::profile() const {
    return *profile_.synchronize();
}

locked_ptr<overseer_t::pool_type>
overseer_t::get_pool() {
    return pool.synchronize();
}

locked_ptr<overseer_t::queue_type>
overseer_t::get_queue() {
    return queue.synchronize();
}

dynamic_t::object_t
overseer_t::info() const {
    dynamic_t::object_t info;

    info["uptime"] = dynamic_t::uint_t(std::chrono::duration_cast<
        std::chrono::seconds
    >(std::chrono::high_resolution_clock::now() - birthstamp).count());

    info["queue"] = dynamic_t::object_t({
        { "depth",    dynamic_t::uint_t(queue->size()) },
        { "capacity", dynamic_t::uint_t(profile().queue_limit) }
    });

    info["channels"] = dynamic_t::object_t({
        { "accepted", dynamic_t::uint_t(stats.accepted) },
        { "rejected", stats.rejected.load() }
    });

    dynamic_t::object_t quantiles;

    char buf[16];

    stats.timings.apply([&](const stats_t::quantiles_t& timings) {
        for (std::size_t i = 0; i < stats.probabilities.size(); ++i) {
            const auto rc = std::snprintf(buf, sizeof(buf) / sizeof(char), "%.2f%%", 100 * stats.probabilities[i]);
            if (rc > 0) {
                const auto quantile = boost::accumulators::extended_p_square(timings)[i];

                quantiles[buf] = std::ceil(quantile * 1000) / 1000;
            }
        }
    });

    info["timings"] = quantiles;

    pool.apply([&](const pool_type& pool) {
        collector_t collector(pool);

        dynamic_t::object_t slaves;
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            const auto stats = it->second.stats();

            slaves[it->first] = dynamic_t::object_t {
                { "uptime", it->second.uptime() },
                { "load", stats.load },
                { "tx",   stats.tx },
                { "rx",   stats.rx },
                { "total", stats.total },
            };
        }

        info["pool"] = dynamic_t::object_t({
            { "active",   collector.active },
            { "idle",     pool.size() - collector.active },
            { "capacity", profile().pool_limit },
            { "slaves", slaves }
        });
    });

    return info;
}

void
overseer_t::balance(std::unique_ptr<balancer_t> balancer) {
    if (balancer) {
        this->balancer = std::move(balancer);
    } else {
        this->balancer.reset(new null_balancer_t);
    }
}

std::shared_ptr<client_rpc_dispatch_t>
overseer_t::enqueue(io::streaming_slot<io::app::enqueue>::upstream_type&& downstream,
                    app::event_t event,
                    boost::optional<slave::id_t> /*id*/)
{
    // TODO: Handle id parameter somehow.

    queue.apply([&](queue_type& queue) {
        const auto limit = profile().queue_limit;

        if (queue.size() >= limit && limit > 0) {
            ++stats.rejected;
            throw std::system_error(error::queue_is_full);
        }
    });

    auto dispatch = std::make_shared<client_rpc_dispatch_t>(manifest.name);

    queue->push({
        std::move(event),
        dispatch,
        std::move(downstream),
    });

    ++stats.accepted;
    balancer->on_queue();

    return dispatch;
}

io::dispatch_ptr_t
overseer_t::prototype() {
    return std::make_shared<const handshaker_t>(
        manifest.name,
        std::bind(&overseer_t::on_handshake, shared_from_this(), ph::_1, ph::_2, ph::_3)
    );
}

void
overseer_t::spawn() {
    spawn(pool.synchronize());
}

void
overseer_t::spawn(locked_ptr<pool_type>& pool) {
    COCAINE_LOG_INFO(log, "enlarging the slaves pool to %d", pool->size() + 1);

    slave_context ctx(context, manifest, profile());

    // It is guaranteed that the cleanup handler will not be invoked from within the slave's
    // constructor.
    const auto uuid = ctx.id;
    pool->insert(std::make_pair(
        uuid,
        slave_t(std::move(ctx), *loop, std::bind(&overseer_t::on_slave_death, shared_from_this(), ph::_1, uuid))
    ));
}

void
overseer_t::spawn(locked_ptr<pool_type>&& pool) {
    spawn(pool);
}

void
overseer_t::assign(slave_t& slave, slave::channel_t& payload) {
    // Attempts to inject the new channel into the slave.
    const auto id = slave.id();
    const auto timestamp = payload.event.birthstamp;
    // TODO: Race possible.
    const auto channel = slave.inject(payload, [=](std::uint64_t channel) {
        const auto now = std::chrono::high_resolution_clock::now();
        const auto elapsed = std::chrono::duration<
            double,
            std::chrono::milliseconds::period
        >(now - timestamp).count();

        COCAINE_LOG_DEBUG(log, "COMPLETED IN %s", elapsed);
        stats.timings.apply([&](stats_t::quantiles_t& timings) {
            timings(elapsed);
        });

        balancer->on_channel_finished(id, channel);
    });
    balancer->on_channel_started(id, channel);
}

void
overseer_t::despawn(const std::string& /*id*/, despawn_policy_t /*policy*/) {
    throw std::runtime_error("overseer_t::despawn: not implemented yet");
}

std::shared_ptr<control_t>
overseer_t::on_handshake(const std::string& id,
                         std::shared_ptr<session_t> session,
                         upstream<io::worker::control_tag>&& stream)
{
    blackhole::scoped_attributes_t holder(*log, {{ "uuid", id }});

    COCAINE_LOG_DEBUG(log, "processing handshake message");

    auto control = pool.apply([&](pool_type& pool) -> std::shared_ptr<control_t> {
        auto it = pool.find(id);
        if (it == pool.end()) {
            COCAINE_LOG_DEBUG(log, "rejecting slave as unexpected");
            return nullptr;
        }

        COCAINE_LOG_DEBUG(log, "activating slave");
        try {
            return it->second.activate(std::move(session), std::move(stream));
        } catch (const std::exception& err) {
            // The slave can be in invalid state; broken, for example, or because the overseer is
            // overloaded. In fact I hope it never happens.
            // Also unlikely we can receive here std::bad_alloc if unable to allocate more memory
            // for control dispatch.
            // If this happens the session will be closed.
            COCAINE_LOG_ERROR(log, "failed to activate the slave: %s", err.what());
        }

        return nullptr;
    });

    if (control) {
        balancer->on_slave_spawn(id);
    }

    return control;
}

void
overseer_t::on_slave_death(const std::error_code& ec, std::string uuid) {
    if (ec) {
        COCAINE_LOG_DEBUG(log, "slave has removed itself from the pool: %s", ec.message());
    } else {
        COCAINE_LOG_DEBUG(log, "slave has removed itself from the pool");
    }

    pool.apply([&](pool_type& pool) {
        auto it = pool.find(uuid);
        if (it != pool.end()) {
            it->second.terminate(ec);
            pool.erase(it);
        }
    });
    balancer->on_slave_death(uuid);
}
