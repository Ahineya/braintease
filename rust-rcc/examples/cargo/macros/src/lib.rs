extern crate proc_macro;
use proc_macro::TokenStream;

#[proc_macro]
pub fn number(_: TokenStream) -> TokenStream {
    "19u16".parse().unwrap()
}
