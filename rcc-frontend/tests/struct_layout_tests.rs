//! Tests for struct layout calculation

use rcc_frontend::semantic::struct_layout::*;
use rcc_frontend::types::{Type, StructField};
use rcc_common::SourceLocation;
use std::collections::HashMap;

#[test]
fn test_simple_struct_layout() {
    let fields = vec![
        StructField {
            name: "x".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "y".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "z".to_string(),
            field_type: Type::Long, // 2 words
            offset: None,
            bit_width: None,
        },
    ];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    assert_eq!(layout.total_size, 4); // 1 + 1 + 2 words
    assert_eq!(layout.fields.len(), 3);

    assert_eq!(layout.fields[0].name, "x");
    assert_eq!(layout.fields[0].offset, 0);
    assert_eq!(layout.fields[0].size, 1);

    assert_eq!(layout.fields[1].name, "y");
    assert_eq!(layout.fields[1].offset, 1);
    assert_eq!(layout.fields[1].size, 1);

    assert_eq!(layout.fields[2].name, "z");
    assert_eq!(layout.fields[2].offset, 2);
    assert_eq!(layout.fields[2].size, 2);
}

#[test]
fn test_struct_with_arrays() {
    let fields = vec![
        StructField {
            name: "arr".to_string(),
            field_type: Type::Array {
                element_type: Box::new(Type::Int),
                size: Some(10),
            },
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "x".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
    ];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    assert_eq!(layout.total_size, 11); // 10 + 1 words
    assert_eq!(layout.fields[0].offset, 0);
    assert_eq!(layout.fields[0].size, 10);
    assert_eq!(layout.fields[1].offset, 10);
    assert_eq!(layout.fields[1].size, 1);
}

#[test]
fn test_struct_with_pointers() {
    let fields = vec![
        StructField {
            name: "ptr".to_string(),
            field_type: Type::Pointer {
                target: Box::new(Type::Int),
                bank: None,
            },
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "x".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
    ];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    assert_eq!(layout.total_size, 3); // 2 (fat ptr) + 1 words
    assert_eq!(layout.fields[0].size, 2); // Fat pointer is 2 words
}

#[test]
fn test_nested_structs() {
    let inner_struct = Type::Struct {
        name: Some("Inner".to_string()),
        fields: vec![
            StructField {
                name: "a".to_string(),
                field_type: Type::Int,
                offset: None,
                bit_width: None,
            },
            StructField {
                name: "b".to_string(),
                field_type: Type::Int,
                offset: None,
                bit_width: None,
            },
        ],
    };

    let fields = vec![
        StructField {
            name: "inner".to_string(),
            field_type: inner_struct,
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "x".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
    ];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    assert_eq!(layout.total_size, 3); // 2 (inner) + 1
    assert_eq!(layout.fields[0].size, 2);
    assert_eq!(layout.fields[0].offset, 0);
    assert_eq!(layout.fields[1].offset, 2);
}

#[test]
fn test_empty_struct() {
    let fields = vec![];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    assert_eq!(layout.total_size, 0);
    assert_eq!(layout.fields.len(), 0);
}

#[test]
fn test_incomplete_type_error() {
    let fields = vec![StructField {
        name: "bad".to_string(),
        field_type: Type::Void, // Void has no size
        offset: None,
        bit_width: None,
    }];

    let result = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0));
    assert!(result.is_err());

    if let Err(err) = result {
        let err_str = format!("{}", err);
        assert!(err_str.contains("incomplete type"));
    }
}

#[test]
fn test_unsized_array_error() {
    let fields = vec![StructField {
        name: "flex".to_string(),
        field_type: Type::Array {
            element_type: Box::new(Type::Int),
            size: None, // Unsized array
        },
        offset: None,
        bit_width: None,
    }];

    let result = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0));
    assert!(result.is_err());
}

#[test]
fn test_find_field() {
    let fields = vec![
        StructField {
            name: "x".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
        StructField {
            name: "y".to_string(),
            field_type: Type::Int,
            offset: None,
            bit_width: None,
        },
    ];

    let layout = calculate_struct_layout(&fields, SourceLocation::new_simple(0, 0))
        .expect("Should calculate layout");

    let field = find_field(&layout, "y").expect("Should find field y");
    assert_eq!(field.name, "y");
    assert_eq!(field.offset, 1);

    assert!(find_field(&layout, "z").is_none());
}

#[test]
fn test_recursive_struct_detection() {
    let mut seen = HashMap::new();

    // Direct recursion - struct contains itself
    let recursive = Type::Struct {
        name: Some("Node".to_string()),
        fields: vec![
            StructField {
                name: "data".to_string(),
                field_type: Type::Int,
                offset: None,
                bit_width: None,
            },
            StructField {
                name: "next".to_string(),
                field_type: Type::Struct {
                    name: Some("Node".to_string()),
                    fields: vec![], // Would be filled in reality
                },
                offset: None,
                bit_width: None,
            },
        ],
    };

    assert!(check_recursive_struct(&recursive, &mut seen));

    // Pointer breaks recursion - this is valid
    seen.clear();
    let valid = Type::Struct {
        name: Some("Node".to_string()),
        fields: vec![
            StructField {
                name: "data".to_string(),
                field_type: Type::Int,
                offset: None,
                bit_width: None,
            },
            StructField {
                name: "next".to_string(),
                field_type: Type::Pointer {
                    target: Box::new(Type::Struct {
                        name: Some("Node".to_string()),
                        fields: vec![],
                    }),
                    bank: None,
                },
                offset: None,
                bit_width: None,
            },
        ],
    };

    assert!(!check_recursive_struct(&valid, &mut seen));
}