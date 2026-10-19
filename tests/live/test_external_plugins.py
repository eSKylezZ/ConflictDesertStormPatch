"""External PSE plugin DLLs in the real game (<game>\PSE\Plugins). The runner copies the SDK examples
example.hello + example.hello_client there and sets PSE_EXAMPLE_PLUGINS=1; otherwise these tests skip."""
import os

import pytest

pytestmark = pytest.mark.skipif(os.environ.get("PSE_EXAMPLE_PLUGINS") != "1",
                                reason="example plugins not deployed (PSE_EXAMPLE_PLUGINS=1)")


def test_examples_loaded_as_external_plugins(c):
    plugins = {p["name"]: p for p in c.run("return dev.pse_plugins()")}
    assert "example.hello" in plugins, sorted(plugins)
    assert "example.hello_client" in plugins
    assert not plugins["example.hello"]["flags"] & 0x80000000   # not PSE_PLUGIN_INTERNAL


def test_load_order_and_cross_plugin_call_in_log(c):
    lines = c.log(5000)
    text = "\n".join(lines) if isinstance(lines, list) else str(lines)
    assert "example.hello" in text
    # hello_client calls example.hello's Add(2, 3) in PostLoad and logs the answer.
    assert "example.hello says 2 + 3 = 5" in text, "no answer line from hello_client"


def test_plugin_frame_subscription_runs(c):
    # hello logs from its PSE_EVENT_FRAME subscription every N frames; frames must keep advancing with it loaded.
    assert c.frames_advancing(1.0)
