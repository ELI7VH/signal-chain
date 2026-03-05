# Gemini CLI Setup

## Install

Copy the Gemini framework files and symlink shared agents:

```bash
cp frameworks/gemini/GEMINI.md /path/to/your/project/GEMINI.md
cp -r frameworks/gemini/.gemini /path/to/your/project/.gemini
ln -s ../../agents /path/to/your/project/agents
```

## Usage

```
/audit       # fingerprint hardware (auto-runs system commands)
/generate    # generate optimal configs from fingerprint
```

The `GEMINI.md` file imports the agent definitions via `@./agents/` references, so they're available in every conversation.

## Structure

Gemini CLI expects:
- `GEMINI.md` — project context (loaded automatically, supports `@./path` imports)
- `.gemini/commands/*.toml` — slash commands with `!{shell}` and `@{file}` injection
- Agent markdown files are imported via `@` references in GEMINI.md
