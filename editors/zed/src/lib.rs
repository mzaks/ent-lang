//! The ent-lang extension for Zed: tells Zed how to start the language
//! server, ent-lsp. The colours and the outline are the grammar's
//! (languages/ent) and need no server.

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

struct EntExtension;

impl zed::Extension for EntExtension {
    fn new() -> Self {
        EntExtension
    }

    /// The server: the one the settings name (`lsp.ent-lsp.binary.path`),
    /// else `ent-lsp` on the PATH, else the one built in the project
    /// (`build/bin/ent-lsp`).
    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(language_server_id.as_ref(), worktree)
            .ok()
            .and_then(|settings| settings.binary);
        let args = binary
            .as_ref()
            .and_then(|binary| binary.arguments.clone())
            .unwrap_or_default();
        let command = binary
            .and_then(|binary| binary.path)
            .or_else(|| worktree.which("ent-lsp"))
            .unwrap_or_else(|| format!("{}/build/bin/ent-lsp", worktree.root_path()));
        Ok(zed::Command {
            command,
            args,
            env: Default::default(),
        })
    }
}

zed::register_extension!(EntExtension);
