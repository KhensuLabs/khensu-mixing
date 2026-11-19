#include <optional>
#include <khensu/ipc/DeepAnalysisRequest.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

juce::String DeepAnalysisRequest::to_json() const
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty(apif::AnalyzeRequest::file_path, juce::String(file_path));
    obj->setProperty(apif::AnalyzeRequest::mode,
                     mode == ILocalServiceBridge::AnalysisMode::Quick ? juce::var("quick") : juce::var("advanced"));
    return juce::JSON::toString(juce::var(obj.release()));
}

std::optional<DeepAnalysisResponse> DeepAnalysisResponse::from_json(const juce::var& json)
{
    if (json[apif::ExtendedAnalysisResponse::bpm].isVoid() || json[apif::ExtendedAnalysisResponse::key].isVoid() || json[apif::ExtendedAnalysisResponse::key_confidence].isVoid() || json[apif::ExtendedAnalysisResponse::sections].isVoid())
        return std::nullopt;

    DeepAnalysisResponse r;
    r.mode = json[apif::ExtendedAnalysisResponse::mode].toString().toStdString();
    r.bpm = static_cast<float>(json[apif::ExtendedAnalysisResponse::bpm]);
    r.bpm_confidence = static_cast<float>(json[apif::ExtendedAnalysisResponse::bpm_confidence]);
    r.key = json[apif::ExtendedAnalysisResponse::key].toString().toStdString();
    r.key_confidence = static_cast<float>(json[apif::ExtendedAnalysisResponse::key_confidence]);

    r.beat_times_json = (json[apif::ExtendedAnalysisResponse::beat_times].isVoid() || json[apif::ExtendedAnalysisResponse::beat_times].isUndefined())
                            ? std::string{}
                            : juce::JSON::toString(json[apif::ExtendedAnalysisResponse::beat_times]).toStdString();
    r.sections_json = juce::JSON::toString(json[apif::ExtendedAnalysisResponse::sections]).toStdString();

    r.downbeat_times_json = (json[apif::ExtendedAnalysisResponse::downbeat_times].isVoid() || json[apif::ExtendedAnalysisResponse::downbeat_times].isUndefined())
                                ? std::string{}
                                : juce::JSON::toString(json[apif::ExtendedAnalysisResponse::downbeat_times]).toStdString();

    r.pitch_notes_json = (json[apif::ExtendedAnalysisResponse::pitch_notes].isVoid() || json[apif::ExtendedAnalysisResponse::pitch_notes].isUndefined())
                             ? std::string{}
                             : juce::JSON::toString(json[apif::ExtendedAnalysisResponse::pitch_notes]).toStdString();

    r.analysis_duration_ms = static_cast<float>(json[apif::ExtendedAnalysisResponse::analysis_duration_ms]);
    r.detected_genre = (json[apif::ExtendedAnalysisResponse::detected_genre].isVoid() || json[apif::ExtendedAnalysisResponse::detected_genre].isUndefined())
                           ? std::string{}
                           : json[apif::ExtendedAnalysisResponse::detected_genre].toString().toStdString();
    r.detected_subgenre = (json[apif::ExtendedAnalysisResponse::detected_subgenre].isVoid() || json[apif::ExtendedAnalysisResponse::detected_subgenre].isUndefined())
                              ? std::string{}
                              : json[apif::ExtendedAnalysisResponse::detected_subgenre].toString().toStdString();

    auto sp06f = [&](const char* k) -> float {
        return (json[k].isVoid() || json[k].isUndefined()) ? 0.0f : static_cast<float>(json[k]);
    };
    r.energy = sp06f("energy");
    r.mood =
        (json[apif::ExtendedAnalysisResponse::mood].isVoid() || json[apif::ExtendedAnalysisResponse::mood].isUndefined()) ? std::string{} : json[apif::ExtendedAnalysisResponse::mood].toString().toStdString();
    // Meter / key relations: null in quick mode (and when the key is unknown).
    auto optStr = [&](const char* k) -> std::string {
        return (json[k].isVoid() || json[k].isUndefined()) ? std::string{} : json[k].toString().toStdString();
    };
    r.time_signature = optStr("time_signature");
    r.meter_confidence = sp06f("meter_confidence");
    r.felt_bpm = sp06f("felt_bpm");
    r.camelot = optStr("camelot");
    r.relative_key = optStr("relative_key");
    r.hook_strength = sp06f("hook_strength");
    r.hook_density = sp06f("hook_density");
    r.hook_count = (json[apif::ExtendedAnalysisResponse::hook_count].isVoid() || json[apif::ExtendedAnalysisResponse::hook_count].isUndefined())
                       ? 0
                       : static_cast<int>(static_cast<double>(json[apif::ExtendedAnalysisResponse::hook_count]));
    r.memorability_score = sp06f("memorability_score");
    r.earworm_likelihood = sp06f("earworm_likelihood");
    r.melody_salience = sp06f("melody_salience");
    r.arc_quality = sp06f("arc_quality");
    r.heaviness_distortion = sp06f("heaviness_distortion");
    r.heaviness_weight = sp06f("heaviness_weight");
    r.heaviness_clarity = sp06f("heaviness_clarity");
    r.heaviness_extremity = sp06f("heaviness_extremity");

    r.skewness =
        (json[apif::ExtendedAnalysisResponse::skewness].isVoid() || json[apif::ExtendedAnalysisResponse::skewness].isUndefined()) ? 0.0f : static_cast<float>(json[apif::ExtendedAnalysisResponse::skewness]);
    r.peak_asymmetry_ratio = (json[apif::ExtendedAnalysisResponse::peak_asymmetry_ratio].isVoid() || json[apif::ExtendedAnalysisResponse::peak_asymmetry_ratio].isUndefined())
                                 ? 1.0f
                                 : static_cast<float>(json[apif::ExtendedAnalysisResponse::peak_asymmetry_ratio]);
    r.is_asymmetric = (json[apif::ExtendedAnalysisResponse::is_asymmetric].isVoid() || json[apif::ExtendedAnalysisResponse::is_asymmetric].isUndefined())
                          ? false
                          : static_cast<bool>(json[apif::ExtendedAnalysisResponse::is_asymmetric]);

    // More music-theory identity (nullable server-side: quick mode, an
    // unparseable key) - null leaves the "unknown" default in place.
    r.parallel_key = optStr("parallel_key");
    if (const auto& cof = json["circle_of_fifths_index"]; cof.isDouble() || cof.isInt() || cof.isInt64())
        r.circle_of_fifths_index = static_cast<int>(static_cast<double>(cof));

    if (!json[apif::ExtendedAnalysisResponse::hook_instances].isVoid() && !json[apif::ExtendedAnalysisResponse::hook_instances].isUndefined() && json[apif::ExtendedAnalysisResponse::hook_instances].isArray()) {
        for (const auto& item : *json[apif::ExtendedAnalysisResponse::hook_instances].getArray()) {
            khensu::HookInstance inst;
            inst.start_sec = static_cast<float>(item[apif::HookInstance::start_sec]);
            inst.end_sec = static_cast<float>(item[apif::HookInstance::end_sec]);
            inst.strength = static_cast<float>(item[apif::HookInstance::strength]);
            auto t = item[apif::HookInstance::hook_type].toString().toStdString();
            inst.type = (t == "topline")    ? khensu::HookType::Topline
                        : (t == "backing")  ? khensu::HookType::Backing
                        : (t == "compound") ? khensu::HookType::Compound
                                            : khensu::HookType::Structural;
            r.hook_instances.push_back(inst);
        }
    }

    // Parse harshness
    if (!json[apif::ExtendedAnalysisResponse::harshness_score].isVoid() && !json[apif::ExtendedAnalysisResponse::harshness_score].isUndefined()) {
        khensu::AnalysisResult::HarshnessData h;
        h.score = static_cast<float>(json[apif::ExtendedAnalysisResponse::harshness_score]);
        auto phz = json[apif::ExtendedAnalysisResponse::harshness_peak_hz];
        if (!phz.isVoid() && !phz.isUndefined() && (phz.isDouble() || phz.isInt() || phz.isInt64())) {
            h.peak_hz = static_cast<float>(phz);
        }
        auto pdb = json[apif::ExtendedAnalysisResponse::harshness_peak_db];
        if (!pdb.isVoid() && !pdb.isUndefined() && (pdb.isDouble() || pdb.isInt() || pdb.isInt64())) {
            h.peak_db = static_cast<float>(pdb);
        }
        r.harshness = h;
    }

    // Parse mono compatibility
    if (!json[apif::ExtendedAnalysisResponse::mono_compat].isVoid() && !json[apif::ExtendedAnalysisResponse::mono_compat].isUndefined() && json[apif::ExtendedAnalysisResponse::mono_compat].isObject()) {
        const auto& mcJson = json[apif::ExtendedAnalysisResponse::mono_compat];
        khensu::AnalysisResult::MonoCompatData mc;
        mc.low_end_stereo_pct = static_cast<float>(mcJson[apif::MonoCompatResult::low_end_stereo_pct]);

        if (const auto* pbcObj = mcJson[apif::MonoCompatResult::per_band_correlation].getDynamicObject()) {
            for (const auto& prop : pbcObj->getProperties()) {
                mc.per_band_correlation[prop.name.toString().toStdString()] = static_cast<float>(prop.value);
            }
        }
        r.mono_compat = mc;
    }

    // Parse translation risk
    if (!json[apif::ExtendedAnalysisResponse::translation_risk].isVoid() && !json[apif::ExtendedAnalysisResponse::translation_risk].isUndefined() &&
        json[apif::ExtendedAnalysisResponse::translation_risk].isObject()) {
        khensu::AnalysisResult::TranslationRiskData tr;
        if (const auto* trObj = json[apif::ExtendedAnalysisResponse::translation_risk].getDynamicObject()) {
            float sum = 0.0f;
            int count = 0;
            for (const auto& prop : trObj->getProperties()) {
                float val = static_cast<float>(prop.value);
                tr.per_environment[prop.name.toString().toStdString()] = val;
                sum += val;
                count++;
            }
            tr.score = count > 0 ? (sum / count) : 0.0f;
        }
        r.translation_risk = tr;
    }

    // Parse resonances
    if (!json[apif::ExtendedAnalysisResponse::resonance_hotspots].isVoid() && !json[apif::ExtendedAnalysisResponse::resonance_hotspots].isUndefined() &&
        json[apif::ExtendedAnalysisResponse::resonance_hotspots].isArray()) {
        khensu::AnalysisResult::ResonanceData res;
        for (const auto& item : *json[apif::ExtendedAnalysisResponse::resonance_hotspots].getArray()) {
            khensu::AnalysisResult::ResonancePeak peak;
            peak.hz = static_cast<float>(item[apif::ResonanceHotspot::hz]);
            peak.db = static_cast<float>(item[apif::ResonanceHotspot::db]);
            peak.prominence = static_cast<float>(item[apif::ResonanceHotspot::prominence_db]);
            res.hotspots.push_back(peak);
        }
        r.resonance = res;
    }

    // Parse mid-side frequency imbalance
    if (const auto& msJson = json[apif::ExtendedAnalysisResponse::mid_side_imbalance];
        !msJson.isVoid() && !msJson.isUndefined() && msJson.isObject()) {
        khensu::AnalysisResult::MidSideImbalanceData ms;
        ms.side_bright_ratio = static_cast<float>(msJson[apif::MidSideImbalanceResult::side_bright_ratio]);
        ms.side_heavy_ratio = static_cast<float>(msJson[apif::MidSideImbalanceResult::side_heavy_ratio]);
        ms.brightness_imbalance_db = static_cast<float>(msJson[apif::MidSideImbalanceResult::brightness_imbalance_db]);
        ms.heaviness_imbalance_db = static_cast<float>(msJson[apif::MidSideImbalanceResult::heaviness_imbalance_db]);
        ms.imbalance_severity = static_cast<float>(msJson[apif::MidSideImbalanceResult::imbalance_severity]);
        auto w = msJson[apif::MidSideImbalanceResult::warning];
        if (!w.isVoid() && !w.isUndefined()) ms.warning = w.toString().toStdString();
        r.mid_side_imbalance = ms;
    }

    // Parse emotional arc
    if (const auto& eaJson = json[apif::ExtendedAnalysisResponse::emotional_arc]; !eaJson.isVoid() && !eaJson.isUndefined() && eaJson.isObject()) {
        khensu::AnalysisResult::EmotionalArcData ea;
        if (auto* pointsArr = eaJson[apif::EmotionalArcData::points].getArray()) {
            for (const auto& p : *pointsArr) {
                khensu::AnalysisResult::EmotionalArcPoint pt;
                pt.window_index = static_cast<int>(static_cast<double>(p[apif::EmotionalArcPoint::window_index]));
                pt.progress_fraction = static_cast<float>(p[apif::EmotionalArcPoint::progress_fraction]);
                pt.energy_0_100 = static_cast<float>(p[apif::EmotionalArcPoint::energy_0_100]);
                pt.mood = p[apif::ExtendedAnalysisResponse::mood].toString().toStdString();
                ea.points.push_back(pt);
            }
        }
        if (auto* flatArr = eaJson[apif::EmotionalArcData::flat_sections].getArray()) {
            for (const auto& fs : *flatArr) {
                if (auto* pair = fs.getArray(); pair && pair->size() == 2) {
                    ea.flat_sections.emplace_back(static_cast<float>((*pair)[0]), static_cast<float>((*pair)[1]));
                }
            }
        }
        ea.max_energy_delta = static_cast<float>(eaJson[apif::EmotionalArcData::max_energy_delta]);
        if (auto* tags = eaJson[apif::EmotionalArcData::track_mood][apif::TrackMood::tags].getArray())
            for (const auto& t : *tags)
                ea.track_mood_tags.push_back(t.toString().toStdString());
        r.emotional_arc = ea;
    }

    // Parse genre blend
    if (!json[apif::ExtendedAnalysisResponse::genre_blend].isVoid() && !json[apif::ExtendedAnalysisResponse::genre_blend].isUndefined() && json[apif::ExtendedAnalysisResponse::genre_blend].isArray()) {
        for (const auto& item : *json[apif::ExtendedAnalysisResponse::genre_blend].getArray()) {
            khensu::AnalysisResult::GenreBlendEntry e;
            e.genre = item[apif::GenreBlendEntry::genre].toString().toStdString();
            e.subgenre = item[apif::GenreBlendEntry::subgenre].toString().toStdString();
            e.rank_score = static_cast<float>(item[apif::GenreBlendEntry::rank_score]);
            r.genre_blend.push_back(e);
        }
    }

    if (const auto& provenance = json[apif::ExtendedAnalysisResponse::genre_provenance]; provenance.isObject())
        r.genre_target_source = provenance["target_source"].toString().toStdString();
    if (const auto& confirmation = json[apif::ExtendedAnalysisResponse::genre_confirmation]; confirmation.isObject())
        r.genre_confirmation_status = confirmation["status"].toString().toStdString();
    r.genre_abstained_reason = (json[apif::ExtendedAnalysisResponse::genre_abstained_reason].isVoid() || json[apif::ExtendedAnalysisResponse::genre_abstained_reason].isUndefined())
                                   ? std::string{}
                                   : json[apif::ExtendedAnalysisResponse::genre_abstained_reason].toString().toStdString();
    r.genre_relative_action_allowed = static_cast<bool>(json[apif::ExtendedAnalysisResponse::genre_relative_action_allowed]);
    r.genre_is_hybrid = static_cast<bool>(json[apif::ExtendedAnalysisResponse::genre_is_hybrid]);

    // Parse translation prediction
    if (const auto& tpJson = json[apif::ExtendedAnalysisResponse::translation_prediction];
        !tpJson.isVoid() && !tpJson.isUndefined() && tpJson.isObject()) {
        khensu::AnalysisResult::TranslationPredictionData tp;
        if (const auto* perProfileObj = tpJson[apif::TranslationPredictionData::per_profile].getDynamicObject()) {
            for (const auto& prop : perProfileObj->getProperties()) {
                const auto& profJson = prop.value;
                khensu::AnalysisResult::TranslationProfileScore score;
                score.degradation_score = static_cast<float>(profJson[apif::TranslationProfileScore::degradation_score]);
                if (auto* collArr = profJson[apif::TranslationProfileScore::new_masking_collisions].getArray()) {
                    for (const auto& c : *collArr) {
                        khensu::AnalysisResult::TranslationCollision coll;
                        if (auto* a = c[apif::TranslationCollision::band_a_hz].getArray(); a && a->size() == 2) {
                            coll.band_a_lo_hz = static_cast<float>((*a)[0]);
                            coll.band_a_hi_hz = static_cast<float>((*a)[1]);
                        }
                        if (auto* b = c[apif::TranslationCollision::band_b_hz].getArray(); b && b->size() == 2) {
                            coll.band_b_lo_hz = static_cast<float>((*b)[0]);
                            coll.band_b_hi_hz = static_cast<float>((*b)[1]);
                        }
                        coll.collision_score = static_cast<float>(c[apif::TranslationCollision::collision_score]);
                        coll.increase = static_cast<float>(c[apif::TranslationCollision::increase]);
                        score.new_masking_collisions.push_back(coll);
                    }
                }
                tp.per_profile[prop.name.toString().toStdString()] = score;
            }
        }
        r.translation_prediction = tp;
    }

    // Parse benchmark
    if (const auto& bJson = json[apif::ExtendedAnalysisResponse::benchmark]; !bJson.isVoid() && !bJson.isUndefined() && bJson.isObject()) {
        khensu::AnalysisResult::BenchmarkData b;
        b.genre_id = bJson[apif::BenchmarkData::genre_id].toString().toStdString();
        b.competitive_summary = bJson[apif::BenchmarkData::competitive_summary].toString().toStdString();
        b.sample_size = static_cast<int>(static_cast<double>(bJson[apif::BenchmarkData::sample_size]));
        b.is_bootstrap_data = static_cast<bool>(bJson[apif::BenchmarkData::is_bootstrap_data]);
        if (const auto* pctObj = bJson[apif::BenchmarkData::percentile_by_metric].getDynamicObject()) {
            for (const auto& prop : pctObj->getProperties())
                b.percentile_by_metric[prop.name.toString().toStdString()] = static_cast<float>(prop.value);
        }
        r.benchmark = b;
    }

    // Parse stereo field timeline
    if (const auto& sfJson = json[apif::ExtendedAnalysisResponse::stereo_field_timeline];
        !sfJson.isVoid() && !sfJson.isUndefined() && sfJson.isObject()) {
        khensu::AnalysisResult::StereoFieldTimeline sf;
        if (auto* pointsArr = sfJson[apif::EmotionalArcData::points].getArray()) {
            for (const auto& p : *pointsArr) {
                khensu::AnalysisResult::StereoFieldPoint pt;
                pt.window_index = static_cast<int>(static_cast<double>(p[apif::EmotionalArcPoint::window_index]));
                pt.progress_fraction = static_cast<float>(p[apif::EmotionalArcPoint::progress_fraction]);
                pt.correlation = static_cast<float>(p[apif::StereoFieldPoint::correlation]);
                pt.width = static_cast<float>(p[apif::StereoFieldPoint::width]);
                pt.sub_bass_mono_pct = static_cast<float>(p[apif::StereoFieldPoint::sub_bass_mono_pct]);
                sf.points.push_back(pt);
            }
        }
        r.stereo_field_timeline = sf;
    }

    return r;
}

void DeepAnalysisResponse::apply_to(khensu::AnalysisResult::PythonData& dst) const noexcept
{
    dst.bpm_detected = bpm;
    dst.bpm_confidence = bpm_confidence;
    dst.pitch_key = key;
    dst.pitch_confidence = key_confidence;
    dst.beat_times_json = beat_times_json;
    dst.downbeat_times_json = downbeat_times_json;
    dst.sections_json = sections_json;
    dst.pitch_notes_json = pitch_notes_json;
    dst.genre_detected = detected_genre;
    dst.subgenre_detected = detected_subgenre;
    dst.energy_detected = energy;
    dst.mood_detected = mood;
    dst.time_signature = time_signature;
    dst.meter_confidence = meter_confidence;
    dst.felt_bpm = felt_bpm;
    dst.camelot = camelot;
    dst.relative_key = relative_key;

    dst.hooks.instances = hook_instances;
    dst.hooks.hook_strength = hook_strength;
    dst.hooks.hook_density = hook_density;
    dst.hooks.hook_count = hook_count;

    dst.memorability.score = memorability_score;
    dst.memorability.earworm_likelihood = earworm_likelihood;
    dst.memorability.melody_salience = melody_salience;

    dst.structural_flow.arc_quality = arc_quality;

    dst.heaviness.distortion = heaviness_distortion;
    dst.heaviness.weight = heaviness_weight;
    dst.heaviness.clarity = heaviness_clarity;
    dst.heaviness.extremity = heaviness_extremity;

    dst.skewness = skewness;
    dst.peak_asymmetry_ratio = peak_asymmetry_ratio;
    dst.is_asymmetric = is_asymmetric;
    dst.circle_of_fifths_index = circle_of_fifths_index;
    dst.parallel_key = parallel_key;
}

std::optional<khensu::ChordAnalysis> DeepAnalysisResponse::chords_from_json(const juce::var& json)
{
    auto* obj = json.getDynamicObject();
    if (obj == nullptr) return std::nullopt;

    auto str = [](const juce::var& v) -> std::string {
        return (v.isVoid() || v.isUndefined() || !v.isString()) ? std::string{} : v.toString().toStdString();
    };

    // Key relations are null when the key is unknown: "" / -1 then.
    khensu::ChordAnalysis ca;
    ca.key = str(obj->getProperty(apif::ChordsResponse::key));
    ca.mode = str(obj->getProperty(apif::ChordsResponse::mode));
    ca.key_confidence = static_cast<float>(obj->getProperty(apif::ChordsResponse::key_confidence));
    ca.camelot = str(obj->getProperty(apif::ChordsResponse::camelot));
    ca.relative_key = str(obj->getProperty(apif::ChordsResponse::relative_key));
    ca.parallel_key = str(obj->getProperty(apif::ChordsResponse::parallel_key));
    const auto cof = obj->getProperty(apif::ChordsResponse::circle_of_fifths_index);
    if (cof.isInt() || cof.isInt64() || cof.isDouble())
        ca.circle_of_fifths_index = static_cast<int>(static_cast<double>(cof));

    if (auto* chordsArr = obj->getProperty(apif::ChordsResponse::chords).getArray()) {
        for (const auto& cv : *chordsArr) {
            khensu::ChordEvent ce;
            ce.time_sec = static_cast<float>(cv[apif::ChordEvent::time_sec]);
            ce.chord = cv[apif::ChordEvent::chord].toString().toStdString();
            if (ce.chord == "N") ce.chord = "N.C."; // service "no chord" class -> standard chart notation
            ce.confidence = static_cast<float>(cv[apif::ChordEvent::confidence]);
            ce.roman = str(cv[apif::ChordEvent::roman]);
            const juce::var diatonic = cv[apif::ChordEvent::diatonic];
            ce.has_diatonic = diatonic.isBool();
            ce.diatonic = ce.has_diatonic && static_cast<bool>(diatonic);
            // Optional harmonic function (null for N.C. / unknown key -> "").
            const juce::var function = cv[apif::ChordEvent::function];
            if (function.isString()) ce.function = function.toString().toStdString();
            ca.chords.push_back(ce);
        }
    }
    return ca;
}

bool engineDidNotAnswer(const BridgeError& e) noexcept
{
    switch (e.kind) {
    case BridgeError::Kind::ServiceUnavailable:
    case BridgeError::Kind::Timeout:
        return true;
    case BridgeError::Kind::HttpError:
        // 429 admission cap, 503 shutting down, 504 a synchronous wrapper
        // timed out (common/errors.py): all "ask again later".
        return e.http_status == 429 || e.http_status == 503 || e.http_status == 504;
    case BridgeError::Kind::ParseError:
        return false;
    }
    return false;
}

} // namespace khensu::ipc
