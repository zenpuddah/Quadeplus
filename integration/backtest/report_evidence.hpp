#pragma once

#include "quant/backtest/host.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace quant::backtest::app {

// Required metadata additions (the wrapper shape and core metrics are unchanged):
// adjustment_policy is checked against the prepared dataset's supported policy;
// adjustment_interpretation explains OHLC/share basis, dividends and AdjClose.
// risk_report_profile="trigger_order_gap_v1"; risk_events_json is a JSON array
// containing zero or one recorded latched trigger. Each entry has trigger,
// orders[], synthetic_terminal (separate, never an executable risk fill), endpoint
// (completed or partial observed state), and outcome. Nullable resolution/fill
// fields mean no such event was recorded, not a hypothetical zero-cost execution.
// Gap P&L = settled(raw opening * trigger-held units) - settled(trigger close *
// those units). Loss=max(0,-gap P&L). Execution drag = raw opening value - executed
// gross + fee. Net exit vs trigger close = gap P&L - execution drag. Dividends are
// excluded from these holding-value diagnostics; none are deducted from P&L again.
// risk_outcome is human-readable, risk_gap_interpretation names these conventions.
// historical_section_profile="continuous_account_through_2025_12_31_v1".
// For a completed fresh scope covering the declared 2025-12-31 session,
// historical_section_json is the FULL native metrics JSON for that ordinary-close
// prefix, with original funding/capital, cumulative postings and terminal Mark.
// No strategy/account is restarted, no synthetic sale or cutoff expiry is added.
// Otherwise it is {status,reason,cutoff_date,metrics:null}: status not_applicable
// (legacy_profile / financial_scope_starts_after_cutoff / ...ends_before_cutoff),
// or unavailable/partial_financial_run. A covering scope missing the actual
// cutoff session rejects. historical_section_summary and an HTML table label
// the view; JSON/CSV metadata retain the complete machine-readable history.

// Required financial report adapter.
// The owner supplies explicit output/resource bounds; no filesystem operations
// occur here. max_rows counts actual suite financial evaluation sessions,
// excluding funding and the numerical/observer warmup prefix. The actual begin
// may be later than the dataset manifest's permitted evaluation_begin.
struct ReportEvidenceSpecs {
    std::filesystem::path output_directory;
    std::uint64_t max_rows = 0;
    std::uint64_t max_artifact_bytes = 0;
    std::vector<std::uint32_t> trial_ids;
    // Trial-major, then HTML / JSON / CSV. Freeze these in PrepareOptions.evidence
    // BEFORE prepare(). Binding checks canonical structure; host.services() checks
    // exact correspondence to the declarations frozen in the prepared plan.
    std::vector<::lexongine::RequiredEvidenceSpec> required;
};

struct ReportEvidenceLabels {
    std::string run_label;
    // Authoritative report label; must agree with legacy_funding_close. Do not
    // infer the profile from the dataset manifest's permitted evaluation range.
    std::string profile;
    // Explicit caller description: e.g. previously_inspected_research_not_holdout
    // or synthetic_fixture. The adapter makes no holdout/acceptance inference.
    std::string window_status;
    std::string provenance;
};

[[nodiscard]] BacktestResult<ReportEvidenceSpecs>
report_specs(const std::filesystem::path &output_directory,
             std::span<const std::uint32_t> trial_ids, std::uint64_t max_rows,
             std::uint64_t max_artifact_bytes) noexcept;

// Call after HostedSimulation::create(), before services()/run(). Pass the SAME
// suite values used for create (retain a copy before moving the suite). Dataset
// copy pins immutable storage; FinancialRun is read only during finalization.
// Actual funding/evaluation dates come from suite keys in that dataset. Its
// permitted financial range is validated and retained as separate metadata;
// numerical origin/end remain the full prepared dataset's origin/end.
// The existing app::make_sink factory owns create-new/write/close/digest actions.
// Required snapshot/cache sinks remain separate and must still be supplied.
//
// Per trial, the first finalizer builds all three artifacts, each <= max bytes;
// subsequent finalizers move them out in enforced trial-major HTML/JSON/CSV order.
// The last returned string's lease transfers to the shared context and is released
// only when the next trial starts serialization, after the previous synchronous
// make_sink finalizer has written/hashed/destroyed its bytes. The final trial's
// lease lasts until context destruction. Explicit reservations: binding Graph
// 64KiB*(trials+1); retained Evidence 3*max bytes for one active/deferred trial;
// transient Scratch 8*max bytes + 4096*max rows + 512*max ledger events + 64KiB.
// Historical reporting adds one native metrics/render pass over <=max_rows,
// after the SAME full ledger validation. Prefix views avoid copying close/trade
// vectors. Historical HTML/CSV are discarded before full-window rendering; only
// its JSON and small summary table survive. Final per-file ceilings include the
// embedded historical JSON (escaped in JSON/CSV/HTML). Quotas are never enlarged
// implicitly; callers must size max_artifact_bytes for both windows before prepare.
// The caller must admit these alongside the existing file/snapshot resources.
// No concurrency is introduced; these callbacks use the synchronous host model.
[[nodiscard]] BacktestResult<std::vector<::lexongine::RequiredEvidenceSink>>
bind_report_sinks(const ReportEvidenceSpecs &, std::shared_ptr<const FinancialRun>,
                  SuiteConfiguration, research::DailyDataset, ReportEvidenceLabels) noexcept;

} // namespace quant::backtest::app
