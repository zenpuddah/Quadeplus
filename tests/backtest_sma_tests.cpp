#include "quant/strategies/policies.hpp"
#include "quant/backtest/arithmetic.hpp"
#include "quant/research/daily_dataset.hpp"
#include "quant/storage/file_integrity.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace {
namespace qb = quant::backtest;
namespace qd = quant::data;
template <class T, class E> T take(std::variant<T, E> value) {
    if (auto *e = std::get_if<E>(&value)) {
        throw std::runtime_error(std::string{e->text()});
    }
    return std::get<T>(std::move(value));
}
void require(bool v, const char *message) {
    if (!v) {
        throw std::runtime_error(message);
    }
}
void fixture(const std::filesystem::path &out, std::string_view mode, unsigned begin) {
    namespace qr = quant::research;
    require(out.is_absolute() && (mode == "trend" || mode == "flat") && (begin == 1 || begin == 60),
            "invalid synthetic fixture request");
    require(std::filesystem::create_directory(out), "fixture output exists");
    std::vector<std::string> dates;
    std::ostringstream csv, grid;
    csv << "Date,Open,High,Low,Close,Volume,AdjClose,Dividend\n";
    grid << "Date\n";
    auto origin = std::chrono::sys_days{std::chrono::year{2004} / 11 / 18};
    for (unsigned i = 0; i < 120; ++i) {
        auto date = std::chrono::year_month_day{origin + std::chrono::days{i}};
        std::ostringstream text;
        text << static_cast<int>(date.year()) << '-' << std::setw(2) << std::setfill('0')
             << static_cast<unsigned>(date.month()) << '-' << std::setw(2)
             << static_cast<unsigned>(date.day());
        dates.push_back(text.str());
        grid << dates.back() << '\n';
        const auto close = mode == "flat" ? 100 : i < 30 ? 100 : i < 70 ? 120 : 80;
        csv << dates.back() << ',' << close + 1 << ',' << close + 2 << ',' << close - 1 << ','
            << close << ",100," << close << ','
            << (i == 50 || i == 60 ? "1.889"
                : i == 73          ? "1"
                                   : "0")
            << '\n';
    }
    auto prices = csv.str(), sessions = grid.str();
    quant::storage::files::write_text(out / "prices.csv", prices);
    quant::storage::files::write_text(out / "sessions.csv", sessions);
    auto account = take(::lexongine::ResourceAccount::create(
        {{16777216, 16777216, 16777216, 16777216, 16777216, 16777216, 8388608},
         1024,
         1024,
         8192,
         128}));
    qr::DatasetSpec spec{"s1-synthetic-v1",
                         "TEST",
                         "USD",
                         "authored-S1-fixture",
                         1,
                         1,
                         1,
                         "synthetic_session_grid_v1",
                         std::string{qr::adjustment_profile},
                         std::string{qr::decimal_profile},
                         std::string{qr::volume_profile},
                         std::string{qr::clock_profile},
                         dates.front(),
                         dates[begin],
                         dates.back()};
    auto prepared = take(qr::prepare_daily_dataset({out / "prices.csv",
                                                    out / "sessions.csv",
                                                    out / "dataset",
                                                    quant::storage::files::sha256(prices),
                                                    quant::storage::files::sha256(sessions),
                                                    spec,
                                                    {1048576, 128, 64},
                                                    account}));
    std::cout << "SYNTHETIC_S1 " << prepared.manifest_sha256() << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 5 && std::string_view{argv[1]} == "--fixture") {
            fixture(argv[2], argv[3], static_cast<unsigned>(std::stoul(argv[4])));
            return 0;
        }
        require(argc == 1, "unknown S1 test arguments");
        auto account = take(qb::fund_account(
            1, {std::string(64, 'a'), 1, 1, 1, qd::CurrencyCode{"USD"}},
            qd::Money::from_raw(qd::CurrencyCode{"USD"}, 1000000), qd::Quantity::from_raw(1)));
        std::vector<qb::PriceFeature> f{{"sma20", qd::Price::from_raw(101), 49, 1},
                                        {"sma50", qd::Price::from_raw(100), 49, 1}};
        const qb::DecisionContext current{49, true, true};
        require(take(quant::strategies::sma20_50_decision(current, &account, f)).kind ==
                    qb::ActionKind::TargetLong,
                "first ready positive spread did not enter");
        f[0].value = qd::Price::from_raw(100);
        require(take(quant::strategies::sma20_50_decision(current, &account, f)).kind ==
                    qb::ActionKind::TargetCash,
                "equality did not target cash");
        f[0].value = qd::Price::from_raw(99);
        require(take(quant::strategies::sma20_50_decision(current, &account, f)).kind ==
                    qb::ActionKind::TargetCash,
                "negative spread did not target cash");
        f[0].value = qd::Price::from_raw(101);
        require(take(quant::strategies::sma20_50_decision({49, false, false}, nullptr, f)).kind ==
                    qb::ActionKind::Hold,
                "warmup traded");
        require(take(quant::strategies::sma20_50_decision(current, &account, {})).kind ==
                    qb::ActionKind::Hold,
                "unready means traded");
        require(std::holds_alternative<qb::BacktestError>(
                    quant::strategies::sma20_50_decision(current, nullptr, f)),
                "missing funded account accepted");
        f[0].key = 50;
        require(std::holds_alternative<qb::BacktestError>(
                    quant::strategies::sma20_50_decision(current, &account, f)),
                "future-key feature accepted");
        f[0].key = 49;
        f.push_back(f.front());
        require(std::holds_alternative<qb::BacktestError>(
                    quant::strategies::sma20_50_decision(current, &account, f)),
                "duplicate feature accepted");
        std::cout << "PASS S1 first-ready, strict comparison, equality, and warmup\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
