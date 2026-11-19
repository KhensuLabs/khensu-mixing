"""Provenance event layer.

An append-only log linking recommendation -> suggested action -> user decision
-> applied change -> resulting file, so the UI can show an honest "what changed
and why" timeline and undo/redo of applied renders. Previously this information
lived in four disconnected places (recommendations in memory, feedback in two
DBs, rendered files on disk, partial suggestion_history).
"""
