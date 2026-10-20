#include "application.hpp"
#include "host_identity.hpp"
#include "suite.hpp"
#include <iomanip>
#include <iostream>
#include <map>
namespace {
namespace qb = quant::backtest;
namespace qr = quant::research;
namespace lx = ::lexongine;
template <class T> T take(std::variant<T, qb::BacktestError> value) {
    if (auto *e = std::get_if<qb::BacktestError>(&value)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(value));
}
std::filesystem::path absolute(const std::string &text) {
    return std::filesystem::absolute(text).lexically_normal();
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--help") {
            std::cout
                << "quadeplus_backtest prepare --research-bundle DIR --symbol SPY "
                   "--schedule-profile retained_reference_grid_v1 --out "
                   "NEW_DIR\nquadeplus_backtest run --config FILE --dataset DIR --out "
                   "NEW_DIR\nquadeplus_backtest suite --config FILE --dataset DIR --out NEW_DIR\n";
            return 0;
        }
        if (argc < 2 || (argc - 2) % 2) {
            throw std::runtime_error(
                "expected a command and explicit option/value pairs; see --help");
        }
        std::map<std::string, std::string> options;
        for (int i = 2; i < argc; i += 2) {
            if (!options.emplace(argv[i], argv[i + 1]).second) {
                throw std::runtime_error("duplicate CLI option");
            }
        }
        const std::string command = argv[1];
        if (command == "prepare") {
            if (options.size() != 4 || !options.contains("--research-bundle") ||
                !options.contains("--symbol") || !options.contains("--schedule-profile") ||
                !options.contains("--out") || options.at("--symbol") != "SPY" ||
                options.at("--schedule-profile") != "retained_reference_grid_v1") {
                throw std::runtime_error("unsupported prepare options/profile");
            }
            auto data = take(qb::app::prepare_spy(absolute(options.at("--research-bundle")),
                                                  absolute(options.at("--out"))));
            std::cout << "PREPARED rows=" << data.rows().size()
                      << " normalizations=" << data.normalizations().size()
                      << " maximum_error=" << data.maximum_normalization_error()
                      << " manifest=" << data.manifest_sha256() << '\n';
            return 0;
        }
        if ((command != "run" && command != "suite") || options.size() != 3 ||
            !options.contains("--config") || !options.contains("--dataset") ||
            !options.contains("--out")) {
            throw std::runtime_error("unsupported run/suite options");
        }
        if (command == "suite") {
            auto inspected =
                qb::inspect_financial_host(qb::app::resources(), {134217728, 1048576, 512});
            if (auto *e = std::get_if<lx::Diagnostic>(&inspected)) {
                std::cerr << "HOST_IDENTITY_FAILURE " << e->text() << '\n';
                return 4;
            }
            auto result = take(
                qb::app::suite(absolute(options.at("--config")), absolute(options.at("--dataset")),
                               absolute(options.at("--out")), std::get<lx::HostBuild>(inspected)));
            std::cout << (result.status == qb::app::SuiteStatus::Accepted      ? "SUITE_ACCEPTED"
                          : result.status == qb::app::SuiteStatus::Unconfirmed ? "SUITE_UNCONFIRMED"
                                                                               : "SUITE_REJECTED")
                      << " children=" << result.completed_children
                      << " manifest=" << result.manifest_sha256
                      << " diagnostic=" << result.diagnostic << '\n';
            return result.status == qb::app::SuiteStatus::Accepted      ? (std::cout.good() ? 0 : 3)
                   : result.status == qb::app::SuiteStatus::Unconfirmed ? 5
                                                                        : 3;
        }
        auto config = take(qb::app::read_config(absolute(options.at("--config"))));
        auto data = take(
            qr::load_daily_dataset({absolute(options.at("--dataset")),
                                    std::nullopt,
                                    {16777216, 8000, 64},
                                    qb::app::resources(config.strategy == "keltner_atr_risk_v1")}));
        auto inspected =
            qb::inspect_financial_host(qb::app::resources(), {134217728, 1048576, 512});
        if (auto *error = std::get_if<lx::Diagnostic>(&inspected)) {
            std::cerr << "HOST_IDENTITY_FAILURE " << error->text() << '\n';
            return 4;
        }
        auto result = take(qb::app::run(data, config, absolute(options.at("--out")),
                                        std::get<lx::HostBuild>(inspected)));
        if (result.host.engine.acceptance != lx::AcceptanceStatus::Accepted) {
            std::cerr << "RUN_REJECTED "
                      << (result.host.engine.cause ? result.host.engine.cause->text()
                                                   : std::string_view{"financial run not accepted"})
                      << '\n';
            return 3;
        }
        std::cout << "RUN_ACCEPTED profile=" << config.profile << " evaluation=" << config.begin
                  << ".." << config.end << " terminal="
                  << (config.terminal == qb::TerminalMode::Mark ? "mark" : "synthetic_close")
                  << '\n';
        for (const auto &s : *result.summaries) {
            std::cout << s.strategy << " equity_cents=" << s.equity << " pnl_cents=" << s.pnl
                      << " realized_cents=" << s.realized << " unrealized_cents=" << s.unrealized
                      << " dividends_cents=" << s.dividends << " fees_cents=" << s.fees
                      << " max_drawdown=" << std::fixed << std::setprecision(4)
                      << 100.0L * static_cast<long double>(s.drawdown_loss) /
                             static_cast<long double>(s.drawdown_peak)
                      << "%\n";
        }
        if (result.total_us) {
            std::cout << "TIMING_US preparation=" << result.preparation_us
                      << " hosted_advance=" << result.advance_us
                      << " finalization=" << result.finalization_us << " total=" << result.total_us
                      << " scope=runner_after_input_and_native_inspection\n";
        }
        return std::cout.good() ? 0 : 3;
    } catch (const std::exception &e) {
        std::cerr << "BACKTEST_ERROR " << e.what() << '\n';
        return 2;
    }
}
