#include <boost/program_options.hpp>

#include <bts/client/client.hpp>
#include <bts/cli/cli.hpp>
#include <bts/rpc/rpc_server.hpp>
#include <bts/lotto/lotto_db.hpp>
#include <bts/lotto/lotto_wallet.hpp>
#include <bts/lotto/lotto_cli.hpp>
#include <bts/lotto/lotto_rpc_server.hpp>
#include <bts/lotto/lotto_delegate.hpp>
#include <fc/filesystem.hpp>
#include <fc/thread/thread.hpp>
#include <fc/log/file_appender.hpp>
#include <fc/log/logger_config.hpp>
#include <fc/io/json.hpp>
#include <fc/reflect/variant.hpp>

#include <iostream>
/// 

// Genesisi address for testing
// private key : c3d0b27f7ca5d2e3158536e97d4001b7b42ab4248000511b252639bd481510e4
// bts address : Jx4he722kVNwNUVwz1f9mjNbQSN5RQHZo
// pts address : Pc26hzt76ErpVvwqSYMEParQ6D5icvQXvo

struct config
{
   config():ignore_console(false){}
   bts::rpc::rpc_server::config rpc;
   bool                         ignore_console;
};

FC_REFLECT( config, (rpc)(ignore_console) )

void print_banner();
void configure_logging(const fc::path&);
fc::path get_data_dir(const boost::program_options::variables_map& option_variables);
config   load_config(const fc::path& datadir);
bts::lotto::lotto_db_ptr load_and_configure_chain_database(const fc::path& datadir,
	const boost::program_options::variables_map& option_variables);

bts::client::client* _global_client = nullptr;

/*
void handle_signal(int signum)
{
    if (_global_client) _global_client->get_wallet()->save();
    exit(1);
}
*/

int main(int argc, char** argv)
{
	// parse command-line options
	boost::program_options::options_description option_config("Allowed options");
	option_config.add_options()("data-dir", boost::program_options::value<std::string>(), "configuration data directory")
		("help", "display this help message")
		("port", boost::program_options::value<uint16_t>(), "set port to listen on")
		("connect-to", boost::program_options::value<std::string>(), "set remote host to connect to")
		("server", "enable JSON-RPC server")
		("rpcuser", boost::program_options::value<std::string>(), "username for JSON-RPC")
		("rpcpassword", boost::program_options::value<std::string>(), "password for JSON-RPC")
		("rpcport", boost::program_options::value<uint16_t>(), "port to listen for JSON-RPC connections")
		("httpport", boost::program_options::value<uint16_t>(), "port to listen for HTTP JSON-RPC connections")
		("trustee-private-key", boost::program_options::value<std::string>(), "act as a trustee using the given private key")
		("trustee-address", boost::program_options::value<std::string>(), "trust the given BTS address to generate blocks")
        ("wallet-pass", boost::program_options::value<std::string>(), "when run as trustee, wallet password must be provided to unlock wallet for import trustee private key")
		("genesis-json", boost::program_options::value<std::string>(), "generate a genesis block with the given json file (only for testing, only accepted when the blockchain is empty)");

	boost::program_options::positional_options_description positional_config;
	positional_config.add("data-dir", 1);

	boost::program_options::variables_map option_variables;
	try
	{
		boost::program_options::store(boost::program_options::command_line_parser(argc, argv).
			options(option_config).positional(positional_config).run(), option_variables);
		boost::program_options::notify(option_variables);
	}
	catch (boost::program_options::error&)
	{
		std::cerr << "Error parsing command-line options\n\n";
		std::cerr << option_config << "\n";
		return 1;
	}

	if (option_variables.count("help"))
	{
		std::cout << option_config << "\n";
		return 0;
	}

	try {
		print_banner();
		fc::path datadir = get_data_dir(option_variables);
		::configure_logging(datadir);

		auto cfg = load_config(datadir);
		auto lotto_db = load_and_configure_chain_database(datadir, option_variables);
        auto wall = std::make_shared<bts::lotto::lotto_wallet>(lotto_db);
		wall->set_data_directory(datadir);

		auto c = std::make_shared<bts::client::client>();
        _global_client = c.get();
		c->set_chain(lotto_db);
		c->set_wallet(wall);
        c->run_delegate();

        bts::lotto::lotto_rpc_server_ptr rpc_server = std::make_shared<bts::lotto::lotto_rpc_server>();
        rpc_server->set_client(c);

        if (option_variables.count("server"))
        {
            // the user wants us to launch the RPC server.
            // First, override any config parameters they 
            bts::rpc::rpc_server::config rpc_config(cfg.rpc);
            if (option_variables.count("rpcuser"))
                rpc_config.rpc_user = option_variables["rpcuser"].as<std::string>();
            if (option_variables.count("rpcpassword"))
                rpc_config.rpc_password = option_variables["rpcpassword"].as<std::string>();
            // for now, force binding to localhost only
            if (option_variables.count("rpcport"))
                rpc_config.rpc_endpoint = fc::ip::endpoint(fc::ip::address("127.0.0.1"), option_variables["rpcport"].as<uint16_t>());
            if (option_variables.count("httpport"))
                rpc_config.httpd_endpoint = fc::ip::endpoint(fc::ip::address("127.0.0.1"), option_variables["httpport"].as<uint16_t>());
            std::cerr << "starting json rpc server on " << std::string(rpc_config.rpc_endpoint) << "\n";
            std::cerr << "starting http json rpc server on " << std::string(rpc_config.httpd_endpoint) << "\n";
            rpc_server->configure(rpc_config);
        }
        else
        {
            std::cout << "Not starting rpc server, use --server to enable the rpc interface\n";
        }

        // TODO: Run lotto delegate instead
        auto lotto_del = std::make_shared<bts::lotto::lotto_delegate>();
        lotto_del->set_lotto_db(lotto_db);
        lotto_del->set_lotto_wallet(wall);
        lotto_del->set_client(c);
		if (option_variables.count("trustee-private-key"))
		{
			auto key = fc::variant(option_variables["trustee-private-key"].as<std::string>()).as<fc::ecc::private_key>();
            
            // TODO: remove useless wallet-pass parameter
            FC_ASSERT(option_variables.count("wallet-pass"));
            lotto_del->run_secret_broadcastor(key, option_variables["wallet-pass"].as<std::string>(), datadir);
			// For testing
            // private key: 733fb1f1a4e00d079fd3506067186168a2bccf45b4fa78d760a12be7f4ba8e0b
            // bts address : XTS6Rzax4UCu67SE19Xet99njVuNBPA9Lc1j
            // pts address : PiNC7Pfj9cmjk9tV1rJ5Kpie6hDEFaRxDb
            // private key : e9624cab9020b00ab1a57a97c9487c7692515c29cdb0ef8444098251a932ad4c
            // bts address : XTSJkNxZ3iboKZPYHGknQjshvXtu5ZKh8FRX
            // pts address : PjjSxKEzJefHKobj96igfBBNdRho25VpbR
		}
		else if (fc::exists("trustee.key"))
		{
			auto key = fc::json::from_file("trustee.key").as<fc::ecc::private_key>();
            
            FC_ASSERT(option_variables.count("wallet-pass"));
            lotto_del->run_secret_broadcastor(key, option_variables["wallet-pass"].as<std::string>(), datadir);
		}

        c->configure(datadir);
        if (option_variables.count("port"))
            c->listen_on_port(option_variables["port"].as<uint16_t>());
        c->connect_to_p2p_network();
        if (option_variables.count("connect-to"))
            c->connect_to_peer(option_variables["connect-to"].as<std::string>());

		auto cli = std::make_shared<bts::lotto::lotto_cli>(c, rpc_server);
		cli->wait();

	}
	catch (const fc::exception& e)
	{
		wlog("${e}", ("e", e.to_detail_string()));
	}
	return 0;
}



void print_banner()
{
	std::cout << "================================================================\n";
	std::cout << "=                                                              =\n";
	std::cout << "=             Welcome to BitShares Lotto                          =\n";
	std::cout << "=                                                              =\n";
	std::cout << "=  This software is in alpha testing and is not suitable for   =\n";
	std::cout << "=  real monetary transactions or trading.  Use at your own     =\n";
	std::cout << "=  risk.                                                       =\n";
	std::cout << "=                                                              =\n";
	std::cout << "=  Type 'help' for usage information.                          =\n";
	std::cout << "================================================================\n";
}

void configure_logging(const fc::path& data_dir)
{
	fc::file_appender::config ac;
	ac.filename = data_dir / "log.txt";
	ac.truncate = false;
	ac.flush = true;
	fc::logging_config cfg;

	cfg.appenders.push_back(fc::appender_config("default", "file", fc::variant(ac)));

	fc::logger_config dlc;
	dlc.level = fc::log_level::debug;
	dlc.name = "default";
	dlc.appenders.push_back("default");
	cfg.loggers.push_back(dlc);
	fc::configure_logging(cfg);
}


fc::path get_data_dir(const boost::program_options::variables_map& option_variables)
{
	try {
		fc::path datadir;
		if (option_variables.count("data-dir"))
		{
			datadir = fc::path(option_variables["data-dir"].as<std::string>().c_str());
		}
		else
		{
#ifdef WIN32
			datadir = fc::app_path() / "BitSharesLotto";
#elif defined( __APPLE__ )
			datadir = fc::app_path() / "BitSharesLotto";
#else
			datadir = fc::app_path() / ".bitshareslotto";
#endif
		}
		return datadir;

	} FC_RETHROW_EXCEPTIONS(warn, "error loading config")
}

bts::lotto::lotto_db_ptr load_and_configure_chain_database(const fc::path& datadir,
	const boost::program_options::variables_map& option_variables)
{
    std::cout << "Loading blockchain from " << (datadir / "chain").generic_string() << "\n";
	auto db = std::make_shared<bts::lotto::lotto_db>();
    fc::optional<fc::path> genesis_file;
    if (option_variables.count("genesis-json"))
        genesis_file = option_variables["genesis-json"].as<std::string>();

    db->open(datadir / "chain", genesis_file);
	return db;
}

config load_config(const fc::path& datadir)
{
	try {
		auto config_file = datadir / "config.json";
		config cfg;
		if (fc::exists(config_file))
		{
			cfg = fc::json::from_file(config_file).as<config>();
		}
		else
		{
			std::cerr << "creating default config file " << config_file.generic_string() << "\n";
			fc::json::save_to_file(cfg, config_file);
		}
		return cfg;
	} FC_RETHROW_EXCEPTIONS(warn, "unable to load config file ${cfg}", ("cfg", datadir / "config.json"))
}