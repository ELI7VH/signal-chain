# Claude Code Setup

## Install

Copy the plugin into your project:

```bash
cp -r frameworks/claude/.claude-plugin /path/to/your/project/
ln -s ../../agents /path/to/your/project/agents
ln -s ../../commands /path/to/your/project/commands
ln -s ../../skills /path/to/your/project/skills
```

Or symlink the entire repo as a plugin directory.

## Usage

```
/signal-chain:audit      # fingerprint hardware
/signal-chain:generate   # generate optimal configs
```

The `hardware-audio-optimization` skill activates automatically when ALSA, ASIO, CoreAudio, or audio latency topics come up.

## Structure

Claude Code expects:
- `.claude-plugin/plugin.json` — plugin metadata
- `agents/*.md` — specialist agent definitions
- `commands/*.md` — slash command definitions (YAML frontmatter + markdown)
- `skills/*.md` — proactive skill definitions (YAML frontmatter + markdown)
