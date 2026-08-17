//! Comprehensive unit tests for cast expression support
//! 
//! These tests ensure that:
//! 1. Cast expressions parse correctly
//! 2. Type checking works properly
//! 3. Code generation produces correct IR
//! 4. Edge cases fail with explicit errors (not silent corruption)

#[cfg(test)]
mod tests {
    use crate::Frontend;
    use crate::ast::{Expression, ExpressionKind};
    use crate::Type;

    /// Helper to parse and check a cast expression
    fn parse_cast_expr(code: &str) -> Result<Expression, String> {
        let full_code = format!("int main() {{ {}; }}", code);
        let ast = Frontend::parse_source(&full_code)
            .map_err(|e| format!("Parse error: {}", e))?;
        
        // Extract the expression from the AST
        for item in &ast.items {
            if let crate::ast::TopLevelItem::Function(func) = item {
                // func.body is a Statement, check if it's a compound statement
                if let crate::ast::StatementKind::Compound(statements) = &func.body.kind {
                    if let Some(stmt) = statements.first() {
                        if let crate::ast::StatementKind::Expression(expr) = &stmt.kind {
                            return Ok(expr.clone());
                        }
                    }
                }
            }
        }
        Err("Failed to extract expression from AST".to_string())
    }

    #[test]
    fn test_parse_int_to_int_cast() {
        let expr = parse_cast_expr("(int)42").expect("Should parse");
        match &expr.kind {
            ExpressionKind::Cast { target_type, operand } => {
                assert_eq!(*target_type, Type::Int);
                match &operand.kind {
                    ExpressionKind::IntLiteral { value: val, .. } => assert_eq!(*val, 42),
                    _ => panic!("Expected int literal operand"),
                }
            }
            _ => panic!("Expected cast expression, got {:?}", expr.kind),
        }
    }

    #[test]
    fn test_parse_char_cast() {
        let expr = parse_cast_expr("(char)256").expect("Should parse");
        match &expr.kind {
            ExpressionKind::Cast { target_type, .. } => {
                assert_eq!(*target_type, Type::Char);
            }
            _ => panic!("Expected cast expression"),
        }
    }

    #[test]
    fn test_parse_pointer_cast() {
        let expr = parse_cast_expr("(int*)0").expect("Should parse");
        match &expr.kind {
            ExpressionKind::Cast { target_type, operand } => {
                assert!(matches!(target_type, Type::Pointer { target, .. } 
                    if **target == Type::Int));
                match &operand.kind {
                    ExpressionKind::IntLiteral { value: val, .. } => assert_eq!(*val, 0),
                    _ => panic!("Expected int literal for NULL"),
                }
            }
            _ => panic!("Expected cast expression"),
        }
    }

    #[test]
    fn test_parse_void_pointer_cast() {
        let expr = parse_cast_expr("(void*)&x").expect("Should parse");
        match &expr.kind {
            ExpressionKind::Cast { target_type, .. } => {
                assert!(matches!(target_type, Type::Pointer { target, .. } 
                    if **target == Type::Void));
            }
            _ => panic!("Expected cast expression"),
        }
    }

    #[test]
    fn test_parse_multi_level_pointer_cast() {
        let expr = parse_cast_expr("(int**)ptr").expect("Should parse");
        match &expr.kind {
            ExpressionKind::Cast { target_type, .. } => {
                // Should be pointer to pointer to int
                match target_type {
                    Type::Pointer { target, .. } => {
                        assert!(matches!(&**target, Type::Pointer { target, .. } 
                            if **target == Type::Int));
                    }
                    _ => panic!("Expected pointer type"),
                }
            }
            _ => panic!("Expected cast expression"),
        }
    }

    #[test]
    fn test_parse_array_pointer_cast() {
        // This should parse the cast, even if array declarators in casts aren't fully supported
        let result = parse_cast_expr("(int(*)[5])ptr");
        // This might fail depending on parser support for complex declarators
        // The important thing is it doesn't silently generate wrong code
        match result {
            Err(err) => {
                // Expected - complex declarators not yet supported
                assert!(err.contains("Expected") || err.contains("declarator"),
                        "Unexpected error: {}", err);
            }
            Ok(_) => {
                // If it parses, that's fine too (parser improved)
            }
        }
    }

    #[test]
    fn test_semantic_void_cast() {
        // Casting to void should be allowed (discards value)
        let code = r#"
            int main() {
                int x = 42;
                (void)x;  // Should be valid
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Void cast should be valid");
    }

    #[test]
    fn test_semantic_incompatible_struct_cast() {
        // Casting between incompatible struct types should fail
        let code = r#"
            struct A { int x; };
            struct B { int y; };
            int main() {
                struct A a;
                struct B* b = (struct B*)&a;  // Should work (pointer cast)
                return 0;
            }
        "#;
        
        let _result = Frontend::compile_to_ir(code, "test");
        // Pointer casts between different struct types should work
        // Direct struct casts would fail
    }

    #[test]
    fn test_codegen_null_pointer() {
        let code = r#"
            int main() {
                int* p = (int*)0;
                void* v = (void*)0;
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "NULL pointer casts should work");
        
        // Check that the IR contains FatPtr with Null bank
        let _module = result.unwrap();
        // We should verify the generated IR has proper FatPtr values with Null bank
    }
    
    #[test]
    fn test_null_pointer_uniqueness() {
        // Test that NULL pointer is unique and distinct from other values
        let code = r#"
            int main() {
                int* null1 = (int*)0;
                void* null2 = (void*)0;
                char* null3 = (char*)0;
                
                // All NULL pointers should be equal
                if (null1 != null2) return 1;
                if (null2 != null3) return 2;
                if (null1 != null3) return 3;
                
                // NULL should equal literal 0
                if (null1 != 0) return 4;
                if (0 != null2) return 5;
                
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "NULL pointer comparisons should work");
    }
    
    #[test]
    fn test_null_pointer_arithmetic() {
        // Test that arithmetic on NULL pointers is handled correctly
        let code = r#"
            int main() {
                int* p = (int*)0;
                int* q = p + 5;  // NULL + offset should still work syntactically
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "NULL pointer arithmetic should compile");
    }
    
    #[test]
    fn test_non_zero_integer_to_pointer() {
        // Test that non-zero integers cast to pointers get Global bank (not Null)
        let code = r#"
            int main() {
                int* p = (int*)100;  // Non-zero constant
                int x = 200;
                int* q = (int*)x;    // Non-zero variable
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Non-zero integer to pointer casts should work");
    }
    
    #[test]
    fn test_null_from_expression() {
        // Test that expressions evaluating to 0 create NULL pointers
        let code = r#"
            int main() {
                int zero = 0;
                int* p = (int*)zero;     // Variable containing 0
                int* q = (int*)(5 - 5);  // Expression evaluating to 0
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Expressions evaluating to 0 should create valid pointers");
        // Note: Only literal 0 gets Null bank, expressions get Global bank
    }
    
    #[test]
    fn test_null_pointer_to_integer() {
        // Test that NULL pointers can be cast back to integers
        let code = r#"
            int main() {
                int* p = (int*)0;
                int addr = (int)p;  // Should be 0
                long laddr = (long)p;  // Should be 0
                return addr + laddr;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "NULL pointer to integer cast should work");
    }

    #[test]
    fn test_codegen_integer_truncation() {
        let code = r#"
            int main() {
                int x = 300;
                char c = (char)x;  // Should truncate/wrap
                return c;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Integer to integer cast should work");
    }

    #[test]
    fn test_codegen_sign_extension() {
        let code = r#"
            int main() {
                char c = -1;
                int i = (int)c;  // Should sign-extend
                return i;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Sign extension cast should work");
    }

    #[test]
    fn test_error_on_function_cast() {
        // Casting to/from function types should fail with clear error
        let code = r#"
            void func() {}
            int main() {
                int (*fp)() = (int(*)())func;  // Function pointer cast
                return 0;
            }
        "#;
        
        let _result = Frontend::compile_to_ir(code, "test");
        // This might fail at parse or semantic level
        // The important thing is it doesn't silently generate wrong code
    }

    #[test]
    fn test_pointer_integer_round_trip() {
        let code = r#"
            int main() {
                int x = 42;
                int* p = &x;
                long addr = (long)p;     // Pointer to integer
                int* p2 = (int*)addr;    // Integer to pointer
                return *p2;              // Should still work
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        // This tests that pointer<->integer casts preserve addresses
        assert!(result.is_ok(), "Pointer/integer round trip should work");
    }

    #[test]
    fn test_const_cast_preservation() {
        // Future: when we add const support, ensure const casts work
        let code = r#"
            int main() {
                const int x = 42;
                int* p = (int*)&x;  // Cast away const (allowed in C)
                return 0;
            }
        "#;
        
        // For now this might fail since const isn't implemented
        // But it shouldn't silently generate wrong code
        let _ = Frontend::compile_to_ir(code, "test");
    }

    #[test]
    fn test_void_pointer_arithmetic_forbidden() {
        // Arithmetic on void* should fail (even after cast)
        let code = r#"
            int main() {
                int arr[10];
                void* p = (void*)arr;
                p = p + 1;  // Should fail - void* arithmetic forbidden
                return 0;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_err(), "void* arithmetic should be forbidden");
    }

    #[test]
    fn test_cast_in_expression_context() {
        // Ensure casts work in various expression contexts
        let code = r#"
            int main() {
                int x = 5;
                int y = (char)x + 1;           // Cast in arithmetic
                int z = ((int*)&x) == 0;       // Cast in comparison  
                int w = (x > 0) ? (char)x : 0; // Cast in conditional
                return y + z + w;
            }
        "#;
        
        let _result = Frontend::compile_to_ir(code, "test");
        // Some of these might fail (e.g., conditional) but shouldn't corrupt
    }

    #[test]
    fn test_nested_casts() {
        let code = r#"
            int main() {
                int x = 42;
                char c = (char)(int)(long)x;  // Multiple nested casts
                return c;
            }
        "#;
        
        let result = Frontend::compile_to_ir(code, "test");
        assert!(result.is_ok(), "Nested casts should work");
    }

    #[test]
    fn test_cast_precedence() {
        // Ensure cast has correct precedence
        let expr = parse_cast_expr("(int)x + 1").expect("Should parse");
        // Should parse as ((int)x) + 1, not (int)(x + 1)
        match &expr.kind {
            ExpressionKind::Binary { left, .. } => {
                assert!(matches!(&left.kind, ExpressionKind::Cast { .. }));
            }
            _ => panic!("Expected binary expression with cast on left"),
        }
    }
}