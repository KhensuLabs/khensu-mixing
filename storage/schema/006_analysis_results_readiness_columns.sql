-- Migration 6: Readiness/safety columns on analysis_results, populated by the
-- Preflight panel's checks (dynamic range, clipping, phase, overall readiness).

ALTER TABLE analysis_results ADD COLUMN dynamic_range_db REAL;
ALTER TABLE analysis_results ADD COLUMN has_clipping INTEGER;
ALTER TABLE analysis_results ADD COLUMN phase_ok INTEGER;
ALTER TABLE analysis_results ADD COLUMN readiness_score REAL;
