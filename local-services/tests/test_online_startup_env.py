from launch_service import apply_offline_env


def test_offline_env_set_when_downloads_disabled():
    env = {}
    apply_offline_env(env, {"online": {"enabled": True, "model_downloads": False, "hf_hub": True}})
    assert env.get("HF_HUB_OFFLINE") == "1"
    assert env.get("TRANSFORMERS_OFFLINE") == "1"


def test_offline_env_when_master_off():
    env = {}
    apply_offline_env(env, {"online": {"enabled": False}})
    assert env.get("HF_HUB_OFFLINE") == "1"


def test_no_offline_env_when_enabled():
    env = {}
    apply_offline_env(env, {"online": {"enabled": True, "model_downloads": True, "hf_hub": True}})
    assert "HF_HUB_OFFLINE" not in env


def test_defaults_online_when_no_settings():
    env = {}
    apply_offline_env(env, {})
    assert "HF_HUB_OFFLINE" not in env
