# Custom / Other Frameworks

The agent definitions in `agents/` are plain markdown. They describe *what to do*, not how any specific framework does it. You can adapt them to any AI coding assistant.

## What You Need

Every framework needs two things:

1. **A way to load the agent context** — copy the relevant `agents/*.md` content into whatever instruction/context mechanism your tool uses
2. **A way to trigger the workflow** — either via slash commands, skills, or just by asking

## Adapting to a New Framework

1. Read the three agent files in `agents/`:
   - `hardware-auditor.md` — what to enumerate and how to structure the output
   - `config-generator.md` — what configs to generate from a fingerprint
   - `optimizer.md` — what diagnostics to run on a live stack

2. Create your framework's manifest/config that points to these files

3. Map the two commands:
   - `audit` — runs the hardware-auditor procedure
   - `generate` — runs the config-generator procedure

4. If your framework supports proactive skills/rules, use `skills/hardware-audio-optimization.md` as the activation logic

## Contributing a New Framework Adapter

Add a directory under `frameworks/<name>/` with:
- The framework's config/manifest files
- A `README.md` explaining setup
- Symlinks or references to the shared `agents/`, `commands/`, `skills/` directories

The agent content stays in one place. The framework adapter is just the wiring.

## Frameworks Without Plugin Systems

If your tool only supports a single instructions file (like a `.cursorrules` or system prompt), concatenate the agent definitions into that file:

```bash
cat agents/hardware-auditor.md agents/config-generator.md agents/optimizer.md > my-instructions.md
```

The content is self-contained and doesn't depend on any framework-specific features.
