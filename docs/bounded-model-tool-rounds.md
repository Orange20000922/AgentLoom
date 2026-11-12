# Bounded model tool rounds

`PersonaRuntimeOptions::max_model_tool_rounds` controls model-initiated tool round trips. The default remains one for compatibility. Consumers that support query rewriting can explicitly select a small bound such as three. Prefetch does not consume this limit.

Synchronous and asynchronous paths use the same bound. Every round retains the existing coordinator, result/provenance validation, required-tool checks, aggregate context budget, cancellation, and shutdown lifecycle. Reaching the limit with another tool call fails the turn without admitting memory. This is not a retry loop or a replacement scheduler.

Streaming generation IDs increment by round. Provisional text is buffered in every tool-capable round and suppressed when that round issues tools, so an intermediate rewritten query does not appear as the final answer.
