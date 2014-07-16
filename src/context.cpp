/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/context.hpp"
#include "cocaine/defaults.hpp"

#include "cocaine/api/service.hpp"

#include "cocaine/asio/reactor.hpp"
#include "cocaine/asio/resolver.hpp"

#include "cocaine/detail/actor.hpp"
#include "cocaine/detail/engine.hpp"
#include "cocaine/detail/essentials.hpp"
#include "cocaine/detail/locator.hpp"

#ifdef COCAINE_ALLOW_RAFT
    #include "cocaine/detail/raft/repository.hpp"
    #include "cocaine/detail/raft/node_service.hpp"
    #include "cocaine/detail/raft/control_service.hpp"
#endif

#include "cocaine/detail/unique_id.hpp"

#include "cocaine/logging.hpp"
#include "cocaine/memory.hpp"

#include <cstring>

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <netdb.h>

#include "rapidjson/reader.h"

#include <blackhole/detail/datetime.hpp>

#include <blackhole/formatter/json.hpp>
#include <blackhole/frontend/files.hpp>
#include <blackhole/frontend/syslog.hpp>
#include <blackhole/repository/config/base.hpp>
#include <blackhole/repository/config/log.hpp>
#include <blackhole/repository/config/parser.hpp>
#include <blackhole/scoped_attributes.hpp>
#include <blackhole/sink/socket.hpp>

using namespace cocaine;
using namespace cocaine::io;

using namespace std::placeholders;

namespace fs = boost::filesystem;

#include "synchronization.inl"

namespace {

struct dynamic_reader_t {
    void
    Null() {
        m_stack.emplace(dynamic_t::null);
    }

    void
    Bool(bool v) {
        m_stack.emplace(v);
    }

    void
    Int(int v) {
        m_stack.emplace(v);
    }

    void
    Uint(unsigned v) {
        m_stack.emplace(dynamic_t::uint_t(v));
    }

    void
    Int64(int64_t v) {
        m_stack.emplace(v);
    }

    void
    Uint64(uint64_t v) {
        m_stack.emplace(dynamic_t::uint_t(v));
    }

    void
    Double(double v) {
        m_stack.emplace(v);
    }

    void
    String(const char* data, size_t size, bool) {
        m_stack.emplace(dynamic_t::string_t(data, size));
    }

    void
    StartObject() {
        // Empty.
    }

    void
    EndObject(size_t size) {
        dynamic_t::object_t object;

        for(size_t i = 0; i < size; ++i) {
            dynamic_t value = std::move(m_stack.top());
            m_stack.pop();

            std::string key = std::move(m_stack.top().as_string());
            m_stack.pop();

            object[key] = std::move(value);
        }

        m_stack.emplace(std::move(object));
    }

    void
    StartArray() {
        // Empty.
    }

    void
    EndArray(size_t size) {
        dynamic_t::array_t array(size);

        for(size_t i = size; i != 0; --i) {
            array[i - 1] = std::move(m_stack.top());
            m_stack.pop();
        }

        m_stack.emplace(std::move(array));
    }

    dynamic_t
    Result() {
        return m_stack.top();
    }

private:
    std::stack<dynamic_t> m_stack;
};

struct rapidjson_ifstream_t {
    rapidjson_ifstream_t(fs::ifstream* backend) :
        m_backend(backend)
    { }

    char
    Peek() const {
        int next = m_backend->peek();

        if(next == std::char_traits<char>::eof()) {
            return '\0';
        } else {
            return next;
        }
    }

    char
    Take() {
        int next = m_backend->get();

        if(next == std::char_traits<char>::eof()) {
            return '\0';
        } else {
            return next;
        }
    }

    size_t
    Tell() const {
        return m_backend->gcount();
    }

    char*
    PutBegin() {
        assert(false);
        return 0;
    }

    void
    Put(char) {
        assert(false);
    }

    size_t
    PutEnd(char*) {
        assert(false);
        return 0;
    }

private:
    fs::ifstream* m_backend;
};

} // namespace

// Config

namespace cocaine {

template<>
struct dynamic_converter<cocaine::config_t::component_t, void> {
    typedef cocaine::config_t::component_t result_type;

    static
    result_type
    convert(const dynamic_t& from) {
        return cocaine::config_t::component_t {
            from.as_object().at("type", "unspecified").as_string(),
            from.as_object().at("args", dynamic_t::empty_object)
        };
    }
};

} // namespace cocaine

// Helpers for dynamic_t to blackhole configuration conversion.

namespace blackhole { namespace repository { namespace config { namespace adapter {

template<class Builder, class Filler>
struct builder_visitor_t : public boost::static_visitor<> {
    const std::string& path;
    const std::string& name;

    Builder& builder;

    builder_visitor_t(const std::string& path, const std::string& name, Builder& builder) :
        path(path),
        name(name),
        builder(builder)
    { }

    void
    operator()(const dynamic_t::null_t&) {
        throw blackhole::error_t("both null and array parsing is not supported");
    }

    template<typename T>
    void
    operator()(const T& value) {
        builder[name] = value;
    }

    void
    operator()(const dynamic_t::array_t&) {
        throw blackhole::error_t("both null and array parsing is not supported");
    }

    void
    operator()(const dynamic_t::object_t& value) {
        auto nested_builder = builder[name];
        Filler::fill(nested_builder, value, path + "/" + name);
    }
};

template<>
struct array_traits<dynamic_t> {
    typedef dynamic_t value_type;
    typedef value_type::array_t::const_iterator const_iterator;

    static
    const_iterator
    begin(const value_type& value) {
        BOOST_ASSERT(value.is_array());
        return value.as_array().begin();
    }

    static
    const_iterator
    end(const value_type& value) {
        BOOST_ASSERT(value.is_array());
        return value.as_array().end();
    }
};

template<>
struct object_traits<dynamic_t> {
    typedef dynamic_t value_type;
    typedef value_type::object_t::const_iterator const_iterator;

    static
    const_iterator
    begin(const value_type& value) {
        BOOST_ASSERT(value.is_object());
        return value.as_object().begin();
    }

    static
    const_iterator
    end(const value_type& value) {
        BOOST_ASSERT(value.is_object());
        return value.as_object().end();
    }

    static
    std::string
    name(const const_iterator& it) {
        return std::string(it->first);
    }

    static
    bool
    has(const value_type& value, const std::string& name) {
        BOOST_ASSERT(value.is_object());
        auto object = value.as_object();
        return object.find(name) != object.end();
    }

    static
    const value_type&
    at(const value_type& value, const std::string& name) {
        BOOST_ASSERT(has(value, name));
        return value.as_object()[name];
    }

    static
    const value_type&
    value(const const_iterator& it) {
        return it->second;
    }

    static
    std::string
    as_string(const value_type& value) {
        BOOST_ASSERT(value.is_string());
        return value.as_string();
    }
};

} // namespace adapter

template<>
struct filler<dynamic_t> {
    typedef adapter::object_traits<dynamic_t> object;

    template<typename T>
    static
    void
    fill(T& builder, const dynamic_t& node, const std::string& path) {
        for(auto it = object::begin(node); it != object::end(node); ++it) {
            const auto& name = object::name(it);
            const auto& value = object::value(it);

            if(name == "type") {
                continue;
            }

            adapter::builder_visitor_t<T, filler<dynamic_t>> visitor {
                path, name, builder
            };

            value.apply(visitor);
        }
    }
};

}}} // namespace blackhole::repository::config

namespace cocaine {

template<>
struct dynamic_converter<cocaine::config_t::logging_t, void> {
    typedef cocaine::config_t::logging_t result_type;

    static
    result_type
    convert(const dynamic_t& from) {
        result_type component;
        const auto& logging = from.as_object();

        for(auto it = logging.begin(); it != logging.end(); ++it) {
            using namespace blackhole::repository;

            auto object = it->second.as_object();
            auto loggers = object.at("loggers", dynamic_t::empty_array);

            config_t::logging_t::logger_t log {
                logmask(object.at("verbosity", defaults::log_verbosity).as_string()),
                object.at("timestamp", defaults::log_timestamp).as_string(),
                config::parser_t<dynamic_t, blackhole::log_config_t>::parse(it->first, loggers)
            };

            component.loggers[it->first] = log;
        }

        return component;
    }

    static inline
    logging::priorities
    logmask(const std::string& verbosity) {
        if(verbosity == "debug") {
            return logging::debug;
        } else if(verbosity == "warning") {
            return logging::warning;
        } else if(verbosity == "error") {
            return logging::error;
        } else {
            return logging::info;
        }
    }
};

} // namespace cocaine

namespace {

// Severity attribute converter from enumeration underlying type into string.

void
map_severity(blackhole::aux::attachable_ostringstream& stream, const logging::priorities& level) {
    static const char* describe[] = {
        "DEBUG",
        "INFO",
        "WARNING",
        "ERROR"
    };

    typedef blackhole::aux::underlying_type<logging::priorities>::type level_type;

    auto value = static_cast<level_type>(level);

    if(value < static_cast<level_type>(sizeof(describe) / sizeof(describe[0])) && value > 0) {
        stream << describe[value];
    } else {
        stream << value;
    }
}

} // namespace

// Mapping trait that is called by Blackhole each time when syslog mapping is required.

namespace blackhole { namespace sink {

template<>
struct priority_traits<logging::priorities> {
    static priority_t map(logging::priorities level) {
        switch (level) {
        case logging::debug:
            return priority_t::debug;
        case logging::info:
            return priority_t::info;
        case logging::warning:
            return priority_t::warning;
        case logging::error:
            return priority_t::err;
        default:
            return priority_t::debug;
        }

        return priority_t::debug;
    }
};

}} // namespace blackhole::sink

config_t::config_t(const std::string& config_path) {
    path.config = config_path;

    const auto config_file_status = fs::status(path.config);

    if(!fs::exists(config_file_status) || !fs::is_regular_file(config_file_status)) {
        throw cocaine::error_t("the configuration file path is invalid");
    }

    fs::ifstream stream(path.config);

    if(!stream) {
        throw cocaine::error_t("unable to read the configuration file");
    }

    rapidjson::MemoryPoolAllocator<> json_allocator;
    rapidjson::Reader json_reader(&json_allocator);
    rapidjson_ifstream_t config_stream(&stream);
    dynamic_reader_t config_constructor;

    if(!json_reader.Parse<rapidjson::kParseDefaultFlags>(config_stream, config_constructor)) {
        if(json_reader.HasParseError()) {
            throw cocaine::error_t("the configuration file is corrupted - %s", json_reader.GetParseError());
        } else {
            throw cocaine::error_t("the configuration file is corrupted");
        }
    }

    const dynamic_t root(config_constructor.Result());

    const auto &path_config    = root.as_object().at("paths", dynamic_t::empty_object).as_object();
    const auto &locator_config = root.as_object().at("locator", dynamic_t::empty_object).as_object();
    const auto &network_config = root.as_object().at("network", dynamic_t::empty_object).as_object();

    // Validation

    if(root.as_object().at("version", 0).to<unsigned int>() != 2) {
        throw cocaine::error_t("the configuration file version is invalid");
    }

    // Paths

    path.plugins = path_config.at("plugins", defaults::plugins_path).as_string();
    path.runtime = path_config.at("runtime", defaults::runtime_path).as_string();

    const auto runtime_path_status = fs::status(path.runtime);

    if(!fs::exists(runtime_path_status)) {
        throw cocaine::error_t("the %s directory does not exist", path.runtime);
    } else if(!fs::is_directory(runtime_path_status)) {
        throw cocaine::error_t("the %s path is not a directory", path.runtime);
    }

    // Hostname configuration

    char hostname[256];

    if(gethostname(hostname, 256) != 0) {
        throw std::system_error(errno, std::system_category(), "unable to determine the hostname");
    }

    addrinfo hints,
             *result = nullptr;

    std::memset(&hints, 0, sizeof(addrinfo));

    hints.ai_flags = AI_CANONNAME;

    const int rv = getaddrinfo(hostname, nullptr, &hints, &result);

    if(rv != 0) {
        throw std::system_error(rv, gai_category(), "unable to determine the hostname");
    }

    network.hostname = locator_config.at("hostname", std::string(result->ai_canonname)).as_string();
    network.uuid     = unique_id_t().string();

    freeaddrinfo(result);

    // Locator configuration

    network.endpoint = locator_config.at("endpoint", defaults::endpoint).as_string();
    network.locator  = locator_config.at("port", defaults::locator_port).to<uint16_t>();

    // WARNING: Now only arrays of two items are allowed.
    auto ports = locator_config.find("port-range");

    if(ports != locator_config.end()) {
        network.ports = ports->second.to<std::tuple<uint16_t, uint16_t>>();
    }

    // Cluster configuration

    if(!network_config.empty()) {
        if(network_config.count("group") == 1) {
            network.group = network_config["group"].as_string();
        }

        if(network_config.count("gateway") == 1) {
            network.gateway = {
                network_config["gateway"].as_object().at("type", "adhoc").as_string(),
                network_config["gateway"].as_object().at("args", dynamic_t::empty_object)
            };
        }
    }

#ifdef COCAINE_ALLOW_RAFT
    create_raft_cluster = false;
#endif

    // Component configuration
    logging  = root.as_object().at("logging" , dynamic_t::empty_object).to<config_t::logging_t>();
    services = root.as_object().at("services", dynamic_t::empty_object).to<config_t::component_map_t>();
    storages = root.as_object().at("storages", dynamic_t::empty_object).to<config_t::component_map_t>();
}

int
config_t::version() {
    return COCAINE_VERSION;
}

// Context

context_t::context_t(config_t config, const std::string& logger_backend):
    config(config)
{
    // Available logging sinks.
    typedef boost::mpl::vector<
        blackhole::sink::files_t<>,
        blackhole::sink::syslog_t<logging::priorities>,
        blackhole::sink::socket_t<boost::asio::ip::tcp>,
        blackhole::sink::socket_t<boost::asio::ip::udp>
    > sinks_t;

    // Available logging formatters.
    typedef boost::mpl::vector<
        blackhole::formatter::string_t,
        blackhole::formatter::json_t
    > formatters_t;

    // Register frontends with all combinations of formatters and sink with the logging repository.
    auto& repository = blackhole::repository_t::instance();
    repository.configure<sinks_t, formatters_t>();

    // Try to initialize the logger. If this fails, there's no way to report
    // the failure, unfortunately, except printing it to the standart output.
    try {
        using blackhole::keyword::tag::timestamp_t;
        using blackhole::keyword::tag::severity_t;

        // Fetch configuration object.
        auto logger = config.logging.loggers.at(logger_backend);

        // Configure some mappings for timestamps and severity attributes.
        blackhole::mapping::value_t mapper;

        mapper.add<severity_t<logging::priorities>>(&map_severity);
        mapper.add<timestamp_t>(logger.timestamp);

        // Attach them to the logging config.
        auto& frontends = logger.config.frontends;

        for(auto it = frontends.begin(); it != frontends.end(); ++it) {
            it->formatter.mapper = mapper;
        }

        // Register logger configuration with the Blackhole's repository.
        repository.add_config(logger.config);

        typedef blackhole::synchronized<logging::logger_t> logger_type;

        // Create the registered logger.
        m_logger = std::make_unique<logger_type>(repository.create<logging::priorities>(logger_backend));
        m_logger->verbosity(logger.verbosity);
    } catch(const std::out_of_range&) {
        throw cocaine::error_t("the '%s' logger is not configured", logger_backend);
    }

#ifdef COCAINE_ALLOW_RAFT
    m_raft = std::make_unique<raft::repository_t>(*this);
#endif

    m_repository.reset(new api::repository_t(*m_logger));

    // Load the builtins.
    essentials::initialize(*m_repository);

    // Load the plugins.
    m_repository->load(config.path.plugins);

    bootstrap();
}

context_t::context_t(config_t config, std::unique_ptr<logging::logger_t>&& logger):
    config(config)
{
    // NOTE: The context takes the ownership of the passed logger, so it will become invalid at the
    // calling site after this call.
    m_logger = std::make_unique<blackhole::synchronized<logging::logger_t>>(std::move(*logger));
    logger.reset();

#ifdef COCAINE_ALLOW_RAFT
    m_raft = std::make_unique<raft::repository_t>(*this);
#endif

    m_repository.reset(new api::repository_t(*m_logger));

    // Load the builtins.
    essentials::initialize(*m_repository);

    // Load the plugins.
    m_repository->load(config.path.plugins);

    bootstrap();
}

context_t::~context_t() {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    COCAINE_LOG_INFO(m_logger, "stopping the synchronization");

    m_synchronization->shutdown();
    m_synchronization.reset();

    auto& unlocked = m_services.value();

    COCAINE_LOG_INFO(m_logger, "stopping the locator");

    unlocked.front().second->terminate();
    unlocked.pop_front();

    COCAINE_LOG_INFO(m_logger, "stopping the services");

    for(auto it = config.services.rbegin(); it != config.services.rend(); ++it) {
        remove(it->first);
    }

    // Any services which haven't been explicitly removed by their owners must be terminated here,
    // because otherwise their thread's destructors will terminate the whole program (13.3.1.3).

    while(!unlocked.empty()) {
        unlocked.back().second->terminate();
        unlocked.pop_back();
    }

    COCAINE_LOG_INFO(m_logger, "stopping the execution units");

    m_pool.clear();
}

std::unique_ptr<logging::log_t>
context_t::log(const std::string& source) {
    return std::make_unique<logging::log_t>(*m_logger, blackhole::log::attributes_t({
        blackhole::keyword::source() = std::move(source)
    }));
}

namespace {

struct match {
    template<class T>
    bool
    operator()(const T& service) const {
        return name == service.first;
    }

    const std::string& name;
};

} // namespace

void
context_t::insert(const std::string& name, std::unique_ptr<actor_t>&& service) {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    uint16_t port = 0;

    {
        auto locked = m_services.synchronize();

        if(std::count_if(locked->begin(), locked->end(), match{name})) {
            throw cocaine::error_t("service '%s' already exists", name);
        }

        if(config.network.ports) {
            if(m_ports.empty()) {
                throw cocaine::error_t("no ports left for allocation");
            }

            port = m_ports.top();

            // NOTE: Remove the taken port from the free pool. If, for any reason, this port is unavailable
            // for binding, it's okay to keep it removed forever.
            m_ports.pop();
        }

        const std::vector<io::tcp::endpoint> endpoints = {{
            boost::asio::ip::address::from_string(config.network.endpoint),
            port
        }};

        service->run(endpoints);

        COCAINE_LOG_INFO(m_logger, "service has been published")(
            "service", name,
            "endpoint", service->location().front()
        );

        locked->emplace_back(name, std::move(service));
    }

    if(m_synchronization) {
        m_synchronization->announce();
    }
}

auto
context_t::remove(const std::string& name) -> std::unique_ptr<actor_t> {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    std::unique_ptr<actor_t> service;

    {
        auto locked = m_services.synchronize();
        auto it = std::find_if(locked->begin(), locked->end(), match{name});

        if(it == locked->end()) {
            throw cocaine::error_t("service '%s' doesn't exist", name);
        }

        // Release the service's actor ownership.
        service = std::move(it->second);

        const std::vector<io::tcp::endpoint> endpoints = service->location();

        service->terminate();

        COCAINE_LOG_INFO(m_logger, "service has been withdrawn")(
            "service", name,
            "endpoint", endpoints.front()
        );

        if(config.network.ports) {
            m_ports.push(endpoints.front().port());
        }

        locked->erase(it);
    }

    if(m_synchronization) {
        m_synchronization->announce();
    }

    return service;
}

auto
context_t::locate(const std::string& name) const -> boost::optional<actor_t&> {
    auto locked = m_services.synchronize();
    auto it = std::find_if(locked->begin(), locked->end(), match{name});

    return boost::optional<actor_t&>(it != locked->end(), *it->second);
}

void
context_t::attach(const std::shared_ptr<io::socket<io::tcp>>& ptr, const std::shared_ptr<io::basic_dispatch_t>& dispatch) {
    m_pool[ptr->fd() % m_pool.size()]->attach(ptr, dispatch);
}

void
context_t::bootstrap() {
    blackhole::scoped_attributes_t guard(
        *m_logger,
        blackhole::log::attributes_t({ blackhole::keyword::source() = "bootstrap" })
    );

    auto pool = boost::thread::hardware_concurrency() * 2;

    if(config.network.ports) {
        uint16_t min, max;

        std::tie(min, max) = config.network.ports.get();

        COCAINE_LOG_INFO(m_logger, "%u ports available, %u through %u", max - min, min, max);

        while(min != max) {
            m_ports.push(--max);
        }
    }

    COCAINE_LOG_INFO(m_logger, "growing the execution unit pool")("units", pool);

    while(pool--) {
        m_pool.emplace_back(std::make_unique<execution_unit_t>(*this, "cocaine/execute"));
    }

    COCAINE_LOG_INFO(m_logger, "starting %d %s", config.services.size(), config.services.size() == 1 ? "service" : "services");

    m_synchronization = std::make_shared<synchronization_t>(*this);

    // Initialize other services.
    for(auto it = config.services.begin(); it != config.services.end(); ++it) {
        auto reactor = std::make_shared<reactor_t>();

        COCAINE_LOG_INFO(m_logger, "starting service")(
            "service", it->first
        );

        try {
            insert(it->first, std::make_unique<actor_t>(*this, reactor, get<api::service_t>(
                it->second.type,
                *this,
                *reactor,
                cocaine::format("service/%s", it->first),
                it->second.args
            )));
        } catch(const std::system_error& e) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service")(
                "service", it->first,
                "reason", e.what(),
                "errno", e.code().value(),
                "message", e.code().message()
            ); throw;
        } catch(const std::exception& e) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service")(
                "service", it->first,
                "reason", e.what()
            ); throw;
        } catch(...) {
            COCAINE_LOG_ERROR(m_logger, "unable to initialize service")(
                "service", it->first,
                "reason", "unknown exception"
            ); throw;
        }
    }

    const std::vector<io::tcp::endpoint> endpoints = {{
        boost::asio::ip::address::from_string(config.network.endpoint),
        config.network.locator
    }};

    COCAINE_LOG_INFO(m_logger, "starting the locator on %s", endpoints.front());

    std::unique_ptr<actor_t> service;

    try {
        auto reactor = std::make_shared<reactor_t>();
        auto locator = std::make_unique<locator_t>(*this, *reactor);

        // Some of the locator methods are better implemented in the Context, to avoid unnecessary
        // copying intermediate structures around, for example service lists synchronization.
        locator->on<io::locator::synchronize>(m_synchronization);

        service = std::make_unique<actor_t>(
            *this,
            reactor,
            std::unique_ptr<basic_dispatch_t>(std::move(locator))
        );

        // NOTE: Start the locator thread last, so that we won't needlessly send node updates to the
        // peers which managed to connect during the bootstrap.
        service->run(endpoints);
    } catch(const std::system_error& e) {
        COCAINE_LOG_ERROR(m_logger, "unable to initialize the locator")(
            "message", e.what(),
            "errno", e.code().value(),
            "reason", e.code().message()
        ); throw;
    } catch(const std::exception& e) {
        COCAINE_LOG_ERROR(m_logger, "unable to initialize the locator")(
            "reason", e.what()
        ); throw;
    } catch(...) {
        COCAINE_LOG_ERROR(m_logger, "unable to initialize the locator")(
            "reason", "unknown exception"
        ); throw;
    }

    m_services->emplace_front("locator", std::move(service));
}
