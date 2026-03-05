# Codex CLI Setup

## Install

Copy the Codex framework files into your project:

```bash
cp frameworks/codex/AGENTS.md /path/to/your/project/AGENTS.md
cp -r frameworks/codex/.codex /path/to/your/project/.codex
```

Symlink the shared agents directory so the skill can reference them:

```bash
ln -s ../../agents /path/to/your/project/agents
```

## Usage

The signal-chain skill activates automatically when Codex detects audio hardware, ALSA, ASIO, or latency topics in your conversation.

You can also reference the agents directly:
- "Run the hardware auditor agent from `agents/hardware-auditor.md`"
- "Use the config generator from `agents/config-generator.md`"

## Structure

Codex CLI expects:
- `AGENTS.md` — project-level instructions (loaded automatically)
- `.codex/skills/<name>/SKILL.md` — skill definition with YAML frontmatter
- Agent markdown files are referenced from within the skill
