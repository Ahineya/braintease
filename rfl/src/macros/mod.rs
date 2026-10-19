//! Compile-time syntax and procedural macro expansion, separate from target execution.
mod executor;
mod expand;
pub(crate) mod protocol;
pub(crate) mod source;
pub(crate) mod syntax;
mod walk;
pub(crate) use expand::prepare;
mod profile;

pub(crate) mod editor;

pub(crate) mod namespace;

pub(crate) use executor::in_host as executor_host;

mod inputs;

#[cfg(test)]
mod tests;
