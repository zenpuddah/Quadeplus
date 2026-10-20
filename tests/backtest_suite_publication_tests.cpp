#include "suite_publication.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
namespace lx = ::lexongine;
namespace fs = std::filesystem;
namespace pub = quant::backtest::app::suite_publication;
using quant::backtest::app::SuiteStatus;
void require(bool value, std::string_view text) {
    if (!value) {
        throw std::runtime_error(std::string{text});
    }
}
template <class T> T take(std::variant<T, lx::Diagnostic> value) {
    if (const auto *error = std::get_if<lx::Diagnostic>(&value)) {
        throw std::runtime_error(std::string{error->text()});
    }
    return std::get<T>(std::move(value));
}
lx::Diagnostic io_error() {
    return lx::diagnostic(lx::FailureCode::EvidenceIO, "injected suite evidence I/O failure");
}
lx::Diagnostic uncertain() {
    return lx::diagnostic(lx::FailureCode::CommitUnconfirmed,
                          "injected missing publish acknowledgment");
}
void overwrite(const fs::path &path, std::string_view text) {
    // Deliberate test-only tampering, never a production repair/overwrite path.
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    require(!output.fail(), "test tamper write/close failed");
}
struct Fixture {
    fs::path root;
    std::shared_ptr<lx::ResourceAccount> account =
        take(lx::ResourceAccount::create({{16 * 1048576, 16 * 1048576, 64 * 1048576, 16 * 1048576,
                                           16 * 1048576, 16 * 1048576, 16 * 1048576},
                                          8192,
                                          4096,
                                          16384,
                                          128}));
    pub::IO native = pub::native_io(account);
    pub::Aggregates aggregates;
    std::vector<pub::File> inventory;
    std::string marker;
    unsigned writes = 0, reads = 0, publishes = 0, after_publish_reads = 0,
             after_publish_writes = 0;
    bool attempted = false;
    explicit Fixture(fs::path path) : root(std::move(path)) {
        require(fs::create_directory(root), "test output must be exclusive");
        require(fs::create_directory(root / "child"), "child directory creation");
        inventory.push_back(
            pub::write_file(root, "child/required.txt", "closed-child-A\n", native));
        // A previously closed child, rechecked by exactly the same helper as suite().
        pub::verify_file(root, inventory.front(), native);
        aggregates = {
            pub::aggregate("registry.csv", "trial,equity\n1,1000000\n"),
            pub::aggregate("cost-comparison.csv", "cost,delta\n5,0\n"),
            pub::aggregate("index.html", "<!doctype html><title>Publication fixture</title>\n"),
            pub::aggregate("manifest.json",
                           "{\"scope\":\"publication_protocol_fixture\",\"child_sha256\":\"" +
                               inventory.front().sha + "\"}\n")};
        for (const auto &item : aggregates) {
            inventory.push_back(item.expected);
        }
        // The helper does not claim this fixture is a financial suite. These bytes
        // test the real publication protocol independently of report generation.
        marker = "{\"schema\":\"suite-publication-test\",\"manifest_sha256\":\"" +
                 aggregates.back().expected.sha + "\"}\n";
    }
    pub::IO observed() {
        return {[this](const fs::path &path,
                       std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                    ++writes;
                    if (attempted) {
                        ++after_publish_writes;
                        throw std::bad_alloc{};
                    }
                    return native.write(path, bytes);
                },
                [this](const fs::path &path, std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                    ++reads;
                    if (attempted) {
                        ++after_publish_reads;
                    }
                    return native.read(path, limit);
                },
                [this](const fs::path &pending, const fs::path &final) -> lx::Result<void> {
                    ++publishes;
                    attempted = true;
                    return native.publish(pending, final);
                }};
    }
    pub::Outcome finish(const pub::IO &io) {
        return pub::finalize(root, inventory, aggregates, marker, io);
    }
    void rejected(pub::Outcome result, pub::Stage stage) {
        require(result.status == SuiteStatus::Rejected && result.stage == stage,
                "wrong prepublication failure outcome/stage");
        require(publishes == 0 && after_publish_reads == 0 && after_publish_writes == 0,
                "prepublication failure reached publication");
        require(!fs::exists(root / "acceptance.json"), "failed prepublication accepted a suite");
        require(fs::is_regular_file(root / "child/required.txt"),
                "failure discarded child evidence");
    }
    void accepted(pub::Outcome result, unsigned reconciliation_count) {
        require(result.status == SuiteStatus::Accepted && result.stage == pub::Stage::None,
                "expected confirmed acceptance");
        require(publishes == 1 && after_publish_reads == reconciliation_count &&
                    after_publish_writes == 0,
                "unexpected postpublication I/O");
        require(take(native.read(root / "acceptance.json", pub::metadata_limit)).value == marker,
                "published marker bytes differ");
        require(!fs::exists(root / "acceptance.pending.json"),
                "rename did not consume pending marker");
    }
    void unconfirmed(pub::Outcome result) {
        require(result.status == SuiteStatus::Unconfirmed && result.stage == pub::Stage::Visibility,
                "unknown visibility was claimed rejected/accepted");
        require(publishes == 1 && after_publish_reads == pub::reconciliation_reads &&
                    after_publish_writes == 0,
                "reconciliation exceeded or skipped its read bound");
    }
};
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: backtest_suite_publication_tests EXISTING_SCRATCH_PARENT");
        const auto parent = fs::canonical(argv[1]);
        require(fs::is_directory(parent), "test scratch parent must exist");
        const auto root = parent / ("suite-publication-" + lx::files::unique_id());
        require(fs::create_directory(root), "test scratch root must be new");
        unsigned passed = 0;
        const auto run = [&](std::string_view name, auto test) {
            Fixture fixture{root / name};
            test(fixture);
            ++passed;
            std::cout << "passed " << name << '\n';
        };
        static_assert(noexcept(pub::finalize(
            std::declval<const fs::path &>(), std::declval<std::span<const pub::File>>(),
            std::declval<const pub::Aggregates &>(), std::declval<std::string_view>(),
            std::declval<const pub::IO &>())));
        static_assert(std::is_trivially_copyable_v<pub::Outcome>);

        run("success_no_postpublish_io", [](Fixture &f) {
            auto io = f.observed();
            const auto read = io.read;
            io.read = [&](const fs::path &path,
                          std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (f.attempted) {
                    ++f.after_publish_reads;
                    throw std::bad_alloc{};
                }
                return read(path, limit);
            };
            f.accepted(f.finish(io), 0);
            require(f.writes == 5 && f.reads == 6,
                    "success bypassed aggregate/complete-inventory/marker closure");
        });
        run("rename_then_error_exact_readback", [](Fixture &f) {
            auto io = f.observed();
            const auto publish = io.publish;
            io.publish = [publish](const fs::path &pending,
                                   const fs::path &final) -> lx::Result<void> {
                take(publish(pending, final));
                return uncertain();
            };
            f.accepted(f.finish(io), 1);
        });
        run("rename_then_throw_exact_readback", [](Fixture &f) {
            auto io = f.observed();
            const auto publish = io.publish;
            io.publish = [publish](const fs::path &pending,
                                   const fs::path &final) -> lx::Result<void> {
                take(publish(pending, final));
                throw std::bad_alloc{};
            };
            f.accepted(f.finish(io), 1);
        });
        run("exact_readback_on_third_attempt", [](Fixture &f) {
            auto io = f.observed();
            const auto publish = io.publish;
            const auto read = io.read;
            io.publish = [publish](const fs::path &pending,
                                   const fs::path &final) -> lx::Result<void> {
                take(publish(pending, final));
                return uncertain();
            };
            io.read = [&](const fs::path &path,
                          std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (f.attempted && f.after_publish_reads < 2) {
                    ++f.after_publish_reads;
                    return io_error();
                }
                return read(path, limit);
            };
            f.accepted(f.finish(io), 3);
        });
        run("missing_final_remains_unconfirmed", [](Fixture &f) {
            auto io = f.observed();
            io.publish = [&](const fs::path &, const fs::path &) -> lx::Result<void> {
                ++f.publishes;
                f.attempted = true;
                return uncertain();
            };
            f.unconfirmed(f.finish(io));
            require(!fs::exists(f.root / "acceptance.json") &&
                        fs::exists(f.root / "acceptance.pending.json"),
                    "missing-final evidence not preserved");
        });
        run("competing_marker_mismatch_no_replace", [](Fixture &f) {
            auto io = f.observed();
            const auto publish = io.publish;
            io.publish = [&](const fs::path &pending, const fs::path &final) -> lx::Result<void> {
                take(f.native.write(final, "different marker\n"));
                return publish(pending, final);
            };
            f.unconfirmed(f.finish(io));
            require(take(f.native.read(f.root / "acceptance.json", pub::metadata_limit)).value ==
                            "different marker\n" &&
                        fs::exists(f.root / "acceptance.pending.json"),
                    "ambiguous no-replace failure overwrote/discarded evidence");
        });
        run("published_but_unreadable_unconfirmed", [](Fixture &f) {
            auto io = f.observed();
            const auto publish = io.publish;
            const auto read = io.read;
            io.publish = [publish](const fs::path &pending,
                                   const fs::path &final) -> lx::Result<void> {
                take(publish(pending, final));
                return uncertain();
            };
            io.read = [&](const fs::path &path,
                          std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (f.attempted) {
                    ++f.after_publish_reads;
                    return io_error();
                }
                return read(path, limit);
            };
            f.unconfirmed(f.finish(io));
            require(fs::exists(f.root / "acceptance.json"),
                    "test must actually publish before losing visibility");
        });
        run("publish_and_reads_throw_unconfirmed", [](Fixture &f) {
            auto io = f.observed();
            const auto read = io.read;
            io.publish = [&](const fs::path &, const fs::path &) -> lx::Result<void> {
                ++f.publishes;
                f.attempted = true;
                throw std::bad_alloc{};
            };
            io.read = [&](const fs::path &path,
                          std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (f.attempted) {
                    ++f.after_publish_reads;
                    throw std::bad_alloc{};
                }
                return read(path, limit);
            };
            f.unconfirmed(f.finish(io));
        });
        run("aggregate_create_write_failure", [](Fixture &f) {
            require(fs::create_directory(f.root / "registry.csv"), "test write obstruction");
            f.rejected(f.finish(f.observed()), pub::Stage::AggregateWrite);
        });
        run("aggregate_partial_write_failure", [](Fixture &f) {
            auto io = f.observed();
            const auto write = io.write;
            io.write = [write](const fs::path &path,
                               std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                if (path.filename() == "cost-comparison.csv") {
                    take(write(path, bytes.substr(0, 3)));
                    return io_error();
                }
                return write(path, bytes);
            };
            f.rejected(f.finish(io), pub::Stage::AggregateWrite);
            require(take(f.native.read(f.root / "cost-comparison.csv", pub::metadata_limit))
                                .value.size() == 3 &&
                        fs::exists(f.root / "registry.csv"),
                    "partial aggregate failure discarded evidence");
        });
        run("aggregate_close_failure", [](Fixture &f) {
            auto io = f.observed();
            const auto write = io.write;
            io.write = [write](const fs::path &path,
                               std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                auto result = write(path, bytes);
                // Failure of the required write/close contract, despite visible bytes.
                // This injects the close-error outcome, not an OS close syscall fault.
                if (path.filename() == "index.html") {
                    take(std::move(result));
                    return io_error();
                }
                return result;
            };
            f.rejected(f.finish(io), pub::Stage::AggregateWrite);
            require(fs::exists(f.root / "index.html"), "close-failure prefix was removed");
        });
        run("aggregate_readback_failure", [](Fixture &f) {
            auto io = f.observed();
            const auto read = io.read;
            io.read = [read](const fs::path &path,
                             std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (path.filename() == "registry.csv") {
                    return io_error();
                }
                return read(path, limit);
            };
            f.rejected(f.finish(io), pub::Stage::Inventory);
            require(fs::exists(f.root / "manifest.json") &&
                        !fs::exists(f.root / "acceptance.pending.json"),
                    "inventory failure reached marker creation");
        });
        run("aggregate_readback_changed_hash", [](Fixture &f) {
            auto io = f.observed();
            const auto write = io.write;
            io.write = [&](const fs::path &path,
                           std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                auto result = write(path, bytes);
                if (path.filename() == "manifest.json") {
                    overwrite(f.root / "registry.csv", "trial,equity\n1,9999999\n");
                }
                return result;
            };
            f.rejected(f.finish(io), pub::Stage::Inventory);
        });
        run("closed_child_changed_before_final_inventory", [](Fixture &f) {
            auto io = f.observed();
            const auto write = io.write;
            io.write = [&](const fs::path &path,
                           std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                auto result = write(path, bytes);
                if (path.filename() == "manifest.json") {
                    overwrite(f.root / "child/required.txt", "closed-child-B\n");
                }
                return result;
            };
            f.rejected(f.finish(io), pub::Stage::Inventory);
            require(fs::file_size(f.root / "child/required.txt") == f.inventory.front().bytes,
                    "child tamper must retain size to exercise hashing");
        });
        run("pending_marker_write_close_failure", [](Fixture &f) {
            auto io = f.observed();
            const auto write = io.write;
            io.write = [write](const fs::path &path,
                               std::string_view bytes) -> lx::Result<lx::EvidenceReference> {
                auto result = write(path, bytes);
                if (path.filename() == "acceptance.pending.json") {
                    take(std::move(result));
                    return io_error();
                }
                return result;
            };
            f.rejected(f.finish(io), pub::Stage::Marker);
            require(fs::exists(f.root / "acceptance.pending.json"),
                    "marker failure lost its prefix");
        });
        run("pending_marker_readback_failure", [](Fixture &f) {
            auto io = f.observed();
            const auto read = io.read;
            io.read = [read](const fs::path &path,
                             std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (path.filename() == "acceptance.pending.json") {
                    return io_error();
                }
                return read(path, limit);
            };
            f.rejected(f.finish(io), pub::Stage::Marker);
        });
        run("prepublication_exception_rejected", [](Fixture &f) {
            auto io = f.observed();
            const auto read = io.read;
            io.read = [read](const fs::path &path,
                             std::uint64_t limit) -> lx::Result<lx::files::Bytes> {
                if (path.filename() == "required.txt") {
                    throw std::bad_alloc{};
                }
                return read(path, limit);
            };
            f.rejected(f.finish(io), pub::Stage::Inventory);
        });
        run("extra_file_rejected", [](Fixture &f) {
            take(f.native.write(f.root / "undeclared.txt", "extra\n"));
            f.rejected(f.finish(f.observed()), pub::Stage::Inventory);
        });
        run("symlink_child_rejected", [](Fixture &f) {
            fs::rename(f.root / "child/required.txt", f.root / "child/retained.txt");
            fs::create_symlink("retained.txt", f.root / "child/required.txt");
            f.rejected(f.finish(f.observed()), pub::Stage::Inventory);
        });
        run("path_escape_rejected", [](Fixture &f) {
            f.inventory.front().path = "../outside.txt";
            f.rejected(f.finish(f.observed()), pub::Stage::Inventory);
        });
        run("metadata_bound_rejected_before_write", [](Fixture &f) {
            f.marker.assign(pub::metadata_limit + 1, 'x');
            f.rejected(f.finish(f.observed()), pub::Stage::Input);
            require(f.writes == 0, "oversized marker wrote aggregates");
        });
        run("inventory_bound_rejected_before_write", [](Fixture &f) {
            f.inventory.resize(513, f.inventory.front());
            f.rejected(f.finish(f.observed()), pub::Stage::Input);
            require(f.writes == 0, "oversized inventory wrote aggregates");
        });
        std::cout << passed << " suite publication cases passed; retained fixtures: " << root
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "suite publication test failed: " << error.what() << '\n';
        return 1;
    }
}
