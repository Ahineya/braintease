// Tests for code generation

#[cfg(test)]
mod tests {
    use crate::Frontend;
    use crate::ir::{Value, IrType};
    use crate::typed_ast::type_translation_unit;
    use crate::codegen::TypedCodeGenerator;
    
    #[test]
    fn test_global_variable_codegen() {
        let source = r#"
            int global_x = 42;
            int global_y = 100;
            int global_uninit;
            
            int main() {
                return global_x;
            }
        "#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();
        
        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();
        
        // Check that globals are in the module
        assert_eq!(module.globals.len(), 3);
        assert_eq!(module.globals[0].name, "global_x");
        assert_eq!(module.globals[1].name, "global_y");
        assert_eq!(module.globals[2].name, "global_uninit");
        
        // Check initializers
        assert!(matches!(module.globals[0].initializer, Some(Value::Constant(42))));
        assert!(matches!(module.globals[1].initializer, Some(Value::Constant(100))));
        assert!(module.globals[2].initializer.is_none());
    }
    
    #[test]
    fn test_string_literal_codegen() {
        let source = r#"
            int main() {
                char *msg = "Hello";
                return 0;
            }
        "#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();
        
        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();
        
        // Check that a string literal global was created
        let string_globals: Vec<_> = module.globals.iter()
            .filter(|g| g.name.starts_with("__str_"))
            .collect();
        
        assert_eq!(string_globals.len(), 1);
        
        // Check that it's an array type
        if let IrType::Array { element_type, size } = &string_globals[0].var_type {
            assert!(matches!(**element_type, IrType::I8));
            assert_eq!(*size, 6); // "Hello" + null terminator
        } else {
            panic!("String literal should be an array type");
        }
        
    }
    
    #[test]
    fn test_multiple_string_literals() {
        let source = r#"
            int main() {
                char *msg1 = "Hi";
                char *msg2 = "Bye";
                return 0;
            }
        "#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();
        
        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();
        
        // Check that two string literal globals were created
        let string_globals: Vec<_> = module.globals.iter()
            .filter(|g| g.name.starts_with("__str_"))
            .collect();
        
        assert_eq!(string_globals.len(), 2);
        
        // Check that they have unique IDs
        let names: Vec<_> = string_globals.iter().map(|g| &g.name).collect();
        assert_ne!(names[0], names[1]);
    }
    
    #[test]
    fn test_global_in_expression() {
        let source = r#"
            int global_x = 10;
            
            int main() {
                int local = global_x + 5;
                global_x = 20;
                return global_x;
            }
        "#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();
        
        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();
        
        // Should compile without errors
        assert_eq!(module.functions.len(), 1);
        assert_eq!(module.globals.len(), 1);
    }
    
    #[test]
    fn test_string_literal_special_chars() {
        let source = r#"
            int main() {
                char *msg = "Hello\nWorld\t!";
                return 0;
            }
        "#;
        
        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();
        
        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();
        
        // Check that string with special chars is handled
        let string_globals: Vec<_> = module.globals.iter()
            .filter(|g| g.name.starts_with("__str_"))
            .collect();
        
        assert_eq!(string_globals.len(), 1);
        
        // Check size includes special chars
        if let IrType::Array { size, .. } = &string_globals[0].var_type {
            assert_eq!(*size, 14); // "Hello\nWorld\t!" + null = 13 + 1
        }
    }

    #[test]
    fn test_unsigned_short_less_emits_ult() {
        let source = r#"
            int cmp(unsigned short a, unsigned short b) {
                return a < b;
            }
        "#;

        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();

        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();

        let has_ult = module.functions.iter().any(|f| {
            f.blocks.iter().any(|b| {
                b.instructions.iter().any(|inst| {
                    matches!(inst, crate::ir::Instruction::Binary { op: crate::ir::IrBinaryOp::Ult, .. })
                })
            })
        });
        let has_slt = module.functions.iter().any(|f| {
            f.blocks.iter().any(|b| {
                b.instructions.iter().any(|inst| {
                    matches!(inst, crate::ir::Instruction::Binary { op: crate::ir::IrBinaryOp::Slt, .. })
                })
            })
        });
        assert!(has_ult, "unsigned short < should lower to ult");
        assert!(!has_slt, "unsigned short < must not lower to slt");
    }

    #[test]
    fn test_extern_then_definition_is_one_global() {
        let source = r#"
            extern unsigned short cur_hi;
            unsigned short cur_hi;

            int main() {
                return cur_hi;
            }
        "#;

        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();

        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();

        let named: Vec<_> = module.globals.iter().filter(|g| g.name == "cur_hi").collect();
        assert_eq!(named.len(), 1, "extern + definition must emit a single global");
    }

    #[test]
    fn test_logical_and_converts_operands_to_bool() {
        let source = r#"
            int f(char c, int i) {
                return c && i < 31;
            }
        "#;

        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();

        let codegen = TypedCodeGenerator::new("test".to_string());
        let module = codegen.generate(&typed_ast).unwrap();

        let mut ne_count = 0;
        let mut and_count = 0;
        for f in &module.functions {
            for b in &f.blocks {
                for inst in &b.instructions {
                    if let crate::ir::Instruction::Binary { op, .. } = inst {
                        match op {
                            crate::ir::IrBinaryOp::Ne => ne_count += 1,
                            crate::ir::IrBinaryOp::And => and_count += 1,
                            _ => {}
                        }
                    }
                }
            }
        }
        assert!(ne_count >= 2, "&& must compare each operand against 0, got {ne_count} ne");
        assert_eq!(and_count, 1, "&& should AND the two boolean results");
    }

    #[test]
    fn test_missing_return_in_non_void_is_error() {
        let source = r#"
            unsigned short peek(unsigned short bank, unsigned short addr) {
                asm("LOAD Rv0, A0, A1");
            }
        "#;

        let mut ast = Frontend::parse_source(source).unwrap();
        let mut analyzer = crate::semantic::SemanticAnalyzer::new();
        analyzer.analyze(&mut ast).unwrap();
        let type_analyzer = analyzer.into_type_info();
        let typed_ast = type_translation_unit(&ast, type_analyzer).unwrap();

        let codegen = TypedCodeGenerator::new("test".to_string());
        let err = codegen.generate(&typed_ast).unwrap_err();
        let msg = format!("{err}");
        assert!(
            msg.contains("falls off the end") || msg.contains("Missing return") || msg.contains("peek"),
            "expected missing-return error, got: {msg}"
        );
    }
}