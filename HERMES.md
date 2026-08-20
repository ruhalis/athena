# Athena

Personal text-only assistant. Brain is ChatGPT via `openai-api` (`OPENAI_API_KEY`). Do not use a fallback provider.

This git repo is the Athena customization layer. Do not edit `~/.hermes/hermes-agent` (upstream). Put our changes here:

- Project rules: this file (`HERMES.md`)
- Skills: `.hermes/skills/<name>/SKILL.md`
- Plugins (tools, hooks): `.hermes/plugins/<name>/plugin.yaml`

## Integrations

### Obsidian

Vault path: `/Users/ruhalis/obsidian/ruhalis` (`OBSIDIAN_VAULT_PATH`).

Use the bundled `obsidian` skill for notes: read, search, create, edit, append, wikilinks. Resolve that absolute path first; never pass `$OBSIDIAN_VAULT_PATH` into file tools.

Layout: `0. Files`, `1. Projects`, `2. Areas`, `Drone`.

### Linear

Linear is connected as the `linear` MCP server (`https://mcp.linear.app/mcp`, OAuth). Use those tools to find, create, and update issues, projects, and comments.

If Linear tools are missing this session, the user still needs `hermes mcp login linear` (browser OAuth), then a new Hermes session.
