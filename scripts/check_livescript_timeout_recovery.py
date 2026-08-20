#!/usr/bin/env python3

from pathlib import Path


source = (Path(__file__).parents[1] / "src/MoonBase/LiveScriptNode.cpp").read_text()


def function_body(signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        depth += source[index] == "{"
        depth -= source[index] == "}"
        if depth == 0:
            return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


quiesce = function_body("bool LiveScriptNode::quiesceTimedOutTasks()")
destructor = function_body("LiveScriptNode::~LiveScriptNode()")
loop = function_body("void LiveScriptNode::loop()")
quarantine_node = function_body("static void quarantineNodeTasks(")
quarantine_task = function_body("static void quarantineTask(")

assert "quarantineNodeTasks(" in quiesce
assert "quarantineNodeTasks(this)" in destructor
for blocking_call in ("->kill(", ".kill(", "kill();", "freeSync(", "portMAX_DELAY"):
    assert blocking_call not in quiesce, f"timeout recovery must not call {blocking_call}"
    assert blocking_call not in destructor, f"destructor must not call {blocking_call}"
    assert blocking_call not in quarantine_node, f"node quarantine must not call {blocking_call}"
    assert blocking_call not in quarantine_task, f"task quarantine must not call {blocking_call}"
assert "vTaskDeleteWithCaps(mapping.task)" in quarantine_task
assert "exec && exec->_isRunning" in quarantine_task
assert "handle && *handle == mapping.task" in quarantine_task
assert "unregisterNodeForTask(mapping.task, mapping.node)" in quarantine_task
assert "unregisterNodeMappings(node)" in quarantine_node
assert "if (gNode == node) gNode = nullptr" in quarantine_node
assert "scriptRuntime.deleteExe(node->animation.c_str())" in quarantine_node
assert "unregisterNodeMappings(this)" in loop

print("LiveScript timeout recovery and destruction bypass the blocking kill path")
