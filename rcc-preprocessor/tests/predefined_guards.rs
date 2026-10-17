use rcc_preprocessor::Preprocessor;
use std::path::PathBuf;

#[test]
fn predefined_macros_agree_across_all_definition_guards() {
    for name in ["__FILE__", "__LINE__", "__STDC__", "__STDC_VERSION__", "__STDC_HOSTED__"] {
        let input = format!(
            "#ifdef {name}\nyes1\n#endif\n#ifndef {name}\nwrong\n#else\nyes2\n#endif\n\
             #if defined({name})\nyes3\n#endif\n"
        );
        let output = Preprocessor::new().process(&input, PathBuf::from("guards.c")).unwrap();
        assert_eq!(output.split_whitespace().collect::<Vec<_>>(), ["yes1", "yes2", "yes3"]);
    }
}
