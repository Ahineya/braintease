use crate::ast::*;
use crate::expander::{ExpansionContext, MacroExpander, replace_whole_word, LocalInfo};
use crate::source_map::SourceMapBuilder;
use crate::types::*;
use std::collections::HashMap;

impl MacroExpander {
    pub fn substitute_in_expression(&self, expr: &ExpressionNode, substitutions: &HashMap<String, String>) -> ExpressionNode {
        match expr {
            ExpressionNode::Identifier(ident) => {
                if let Some(value) = substitutions.get(&ident.name) {
                    let trimmed = value.trim();
                    if let Ok(num) = if trimmed.starts_with("0x") || trimmed.starts_with("0X") {
                        i64::from_str_radix(&trimmed[2..], 16)
                    } else {
                        trimmed.parse()
                    } {
                        return ExpressionNode::Number(NumberNode {
                            value: num,
                            position: expr.position().clone(),
                        });
                    }
                    return ExpressionNode::Text(TextNode {
                        value: value.clone(),
                        position: expr.position().clone(),
                    });
                }
                expr.clone()
            }
            ExpressionNode::Text(text_node) => {
                let text = &text_node.value;
                
                if let Some(value) = substitutions.get(text) {
                    let trimmed = value.trim();
                    if let Ok(num) = if trimmed.starts_with("0x") || trimmed.starts_with("0X") {
                        i64::from_str_radix(&trimmed[2..], 16)
                    } else {
                        trimmed.parse()
                    } {
                        return ExpressionNode::Number(NumberNode {
                            value: num,
                            position: text_node.position.clone(),
                        });
                    }
                    return ExpressionNode::Text(TextNode {
                        value: value.clone(),
                        position: text_node.position.clone(),
                    });
                }
                
                let mut result_text = text.clone();
                let mut sorted_subs: Vec<_> = substitutions.iter().collect();
                sorted_subs.sort_by_key(|(k, _)| std::cmp::Reverse(k.len()));
                
                for (param, value) in sorted_subs {
                    result_text = replace_whole_word(&result_text, param, value);
                }
                
                if result_text != text_node.value {
                    ExpressionNode::Text(TextNode {
                        value: result_text,
                        position: text_node.position.clone(),
                    })
                } else {
                    expr.clone()
                }
            }
            ExpressionNode::ExpressionList(list) => {
                let expressions = list.expressions.iter().map(|item| {
                    match item {
                        ContentNode::MacroInvocation(inv) => {
                            let substituted = self.substitute_in_expression(
                                &ExpressionNode::MacroInvocation(inv.clone()),
                                substitutions
                            );
                            if let ExpressionNode::MacroInvocation(new_inv) = substituted {
                                ContentNode::MacroInvocation(new_inv)
                            } else {
                                item.clone()
                            }
                        }
                        ContentNode::BuiltinFunction(builtin) => {
                            let substituted = self.substitute_in_expression(
                                &ExpressionNode::BuiltinFunction(builtin.clone()),
                                substitutions
                            );
                            if let ExpressionNode::BuiltinFunction(new_builtin) = substituted {
                                ContentNode::BuiltinFunction(new_builtin)
                            } else {
                                item.clone()
                            }
                        }
                        ContentNode::Text(text) => {
                            let substituted = self.substitute_in_expression(
                                &ExpressionNode::Text(text.clone()),
                                substitutions
                            );
                            if let ExpressionNode::Text(new_text) = substituted {
                                ContentNode::Text(new_text)
                            } else {
                                item.clone()
                            }
                        }
                        _ => item.clone()
                    }
                }).collect();
                
                ExpressionNode::ExpressionList(ExpressionListNode {
                    expressions,
                    position: list.position.clone(),
                })
            }
            ExpressionNode::BuiltinFunction(builtin) => {
                let arguments = builtin.arguments.iter()
                    .map(|arg| self.substitute_in_expression(arg, substitutions))
                    .collect();
                ExpressionNode::BuiltinFunction(BuiltinFunctionNode {
                    name: builtin.name.clone(),
                    arguments,
                    position: builtin.position.clone(),
                })
            }
            ExpressionNode::MacroInvocation(invocation) => {
                let arguments = invocation.arguments.as_ref().map(|args| {
                    args.iter()
                        .map(|arg| self.substitute_in_expression(arg, substitutions))
                        .collect()
                });
                ExpressionNode::MacroInvocation(MacroInvocationNode {
                    name: invocation.name.clone(),
                    arguments,
                    position: invocation.position.clone(),
                })
            }
            ExpressionNode::ArrayLiteral(array) => {
                let elements = array.elements.iter()
                    .map(|el| self.substitute_in_expression(el, substitutions))
                    .collect();
                ExpressionNode::ArrayLiteral(ArrayLiteralNode {
                    elements,
                    position: array.position.clone(),
                })
            }
            ExpressionNode::ForPattern(pattern) => {
                // Substitute within the inner pattern
                let substituted_var = self.substitute_in_expression(pattern.variable.as_ref(), substitutions);
                ExpressionNode::ForPattern(ForPatternNode {
                    variable: Box::new(substituted_var),
                    index_variable: pattern.index_variable.clone(),
                    position: pattern.position.clone(),
                })
            }
            _ => expr.clone()
        }
    }
    
    pub fn expand_builtin_function(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        match node.name {
            BuiltinFunction::Repeat => self.expand_repeat(node, context, generate_source_map, source_range),
            BuiltinFunction::If => self.expand_if(node, context, generate_source_map, source_range),
            BuiltinFunction::For => self.expand_for(node, context, generate_source_map, source_range),
            BuiltinFunction::Reverse => self.expand_reverse(node, context, generate_source_map, source_range),
            BuiltinFunction::Preserve => self.expand_preserve(node, context, generate_source_map, source_range),
            BuiltinFunction::Label => self.expand_label(node, context, generate_source_map, source_range),
            BuiltinFunction::Br => self.expand_br(node, context, generate_source_map, source_range),
            BuiltinFunction::Proc => self.expand_proc(node, context, generate_source_map, source_range),
            BuiltinFunction::Local => self.expand_local(node, context, generate_source_map, source_range),
            BuiltinFunction::Len => self.expand_len(node, context, generate_source_map, source_range),
        }
    }
    
    fn expand_repeat(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 2 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("repeat() expects exactly 2 arguments, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        let count_expr = self.expand_expression_to_string(&node.arguments[0], context);
        let trimmed = count_expr.trim();
        
        let count = if trimmed.starts_with("'") && trimmed.ends_with("'") && trimmed.len() == 3 {
            trimmed.chars().nth(1).unwrap() as i64
        } else if trimmed.starts_with("0x") || trimmed.starts_with("0X") {
            i64::from_str_radix(&trimmed[2..], 16).unwrap_or(-1)
        } else {
            trimmed.parse().unwrap_or(-1)
        };
        
        if count < 0 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("Invalid repeat count: {}", count_expr),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        let current_macro = context.macro_call_stack.last();
        let effective_source_range = current_macro.as_ref()
            .map(|m| m.call_site.clone())
            .unwrap_or(source_range);
        
        for _ in 0..count {
            if let ExpressionNode::BrainfuckCommand(cmd) = &node.arguments[1] {
                self.append_to_expanded(&cmd.commands, context, generate_source_map, Some(effective_source_range.clone()));
            } else {
                self.expand_expression(&node.arguments[1], context, generate_source_map);
            }
        }
    }
    
    fn expand_if(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 3 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("if() expects exactly 3 arguments, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        let condition_expr = self.expand_expression_to_string(&node.arguments[0], context);
        let trimmed = condition_expr.trim();
        
        let condition = if trimmed.starts_with("'") && trimmed.ends_with("'") && trimmed.len() == 3 {
            trimmed.chars().nth(1).unwrap() as i64
        } else if trimmed.starts_with("0x") || trimmed.starts_with("0X") {
            i64::from_str_radix(&trimmed[2..], 16).unwrap_or(0)
        } else {
            trimmed.parse().unwrap_or(0)
        };
        
        let selected_branch = if condition != 0 { &node.arguments[1] } else { &node.arguments[2] };
        self.expand_expression(selected_branch, context, generate_source_map);
    }
    
    fn expand_for(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, _source_range: PositionRange) {
        if node.arguments.len() != 3 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("for() expects exactly 3 arguments, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        let var_node = &node.arguments[0];
        let array_node = &node.arguments[1];
        let body_node = &node.arguments[2];
        
        let (var_names, index_var) = match var_node {
            ExpressionNode::Identifier(ident) => (vec![ident.name.clone()], None),
            ExpressionNode::TuplePattern(tuple) => (tuple.elements.clone(), None),
            ExpressionNode::ForPattern(pattern) => {
                // Extract variable names from the inner pattern
                let inner_names = match pattern.variable.as_ref() {
                    ExpressionNode::Identifier(ident) => vec![ident.name.clone()],
                    ExpressionNode::TuplePattern(tuple) => tuple.elements.clone(),
                    _ => {
                        self.errors.push(MacroExpansionError {
                            error_type: MacroExpansionErrorType::SyntaxError,
                            message: format!("Invalid variable pattern in for loop"),
                            location: Some(SourceLocation {
                                line: node.position.line.saturating_sub(1),
                                column: node.position.column.saturating_sub(1),
                                length: node.position.end - node.position.start,
                            }),
                        });
                        return;
                    }
                };
                (inner_names, pattern.index_variable.clone())
            }
            _ => {
                self.errors.push(MacroExpansionError {
                    error_type: MacroExpansionErrorType::SyntaxError,
                    message: format!("Expected variable name or tuple pattern in for loop, got {:?}", var_node),
                    location: Some(SourceLocation {
                        line: node.position.line.saturating_sub(1),
                        column: node.position.column.saturating_sub(1),
                        length: node.position.end - node.position.start,
                    }),
                });
                return;
            }
        };
        
        let is_tuple_pattern = match var_node {
            ExpressionNode::TuplePattern(_) => true,
            ExpressionNode::ForPattern(pattern) => matches!(pattern.variable.as_ref(), ExpressionNode::TuplePattern(_)),
            _ => false,
        };
        
        let values = self.extract_array_values(array_node, context, is_tuple_pattern);
        
        for (index, value) in values.into_iter().enumerate() {
            let mut temp_substitutions = HashMap::new();
            
            // Add index variable if present
            if let Some(idx_var) = &index_var {
                temp_substitutions.insert(idx_var.clone(), index.to_string());
            }
            
            if is_tuple_pattern {
                let tuple_elements = self.parse_tuple_elements(&value);
                for (i, var_name) in var_names.iter().enumerate() {
                    temp_substitutions.insert(
                        var_name.clone(),
                        tuple_elements.get(i).cloned().unwrap_or_default()
                    );
                }
            } else {
                temp_substitutions.insert(var_names[0].clone(), value);
            }
            
            match body_node {
                ExpressionNode::ExpressionList(list) => {
                    for expr in &list.expressions {
                        let substituted = match expr {
                            ContentNode::MacroInvocation(inv) => {
                                self.substitute_in_expression(
                                    &ExpressionNode::MacroInvocation(inv.clone()),
                                    &temp_substitutions
                                )
                            }
                            ContentNode::BuiltinFunction(builtin) => {
                                self.substitute_in_expression(
                                    &ExpressionNode::BuiltinFunction(builtin.clone()),
                                    &temp_substitutions
                                )
                            }
                            ContentNode::Text(text) => {
                                self.substitute_in_expression(
                                    &ExpressionNode::Text(text.clone()),
                                    &temp_substitutions
                                )
                            }
                            ContentNode::BrainfuckCommand(cmd) => {
                                ExpressionNode::BrainfuckCommand(cmd.clone())
                            }
                        };
                        self.expand_expression(&substituted, context, generate_source_map);
                    }
                }
                _ => {
                    let substituted = self.substitute_in_expression(body_node, &temp_substitutions);
                    self.expand_expression(&substituted, context, generate_source_map);
                }
            }
        }
    }
    
    fn expand_reverse(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 1 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("reverse() expects exactly 1 argument, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        let array_arg = &node.arguments[0];
        
        match array_arg {
            ExpressionNode::ArrayLiteral(array_literal) => {
                let mut reversed_elements: Vec<_> = array_literal.elements.clone();
                reversed_elements.reverse();
                let expanded_elements: Vec<String> = reversed_elements.iter()
                    .map(|el| self.expand_expression_to_string(el, context))
                    .collect();
                let result = format!("{{{}}}", expanded_elements.join(", "));
                self.append_to_expanded(&result, context, generate_source_map, Some(source_range));
            }
            _ => {
                let expanded = self.expand_expression_to_string(array_arg, context).trim().to_string();
                
                if expanded.starts_with('{') && expanded.ends_with('}') {
                    let inner = &expanded[1..expanded.len()-1];
                    let mut values: Vec<_> = inner.split(',').map(|v| v.trim()).collect();
                    values.reverse();
                    let result = format!("{{{}}}", values.join(", "));
                    self.append_to_expanded(&result, context, generate_source_map, Some(source_range));
                } else if expanded.contains(',') {
                    let mut values: Vec<_> = expanded.split(',').map(|v| v.trim()).collect();
                    values.reverse();
                    let result = format!("{{{}}}", values.join(", "));
                    self.append_to_expanded(&result, context, generate_source_map, Some(source_range));
                } else {
                    self.errors.push(MacroExpansionError {
                        error_type: MacroExpansionErrorType::SyntaxError,
                        message: format!("reverse() expects an array literal, got {:?}", array_arg),
                        location: Some(SourceLocation {
                            line: node.position.line.saturating_sub(1),
                            column: node.position.column.saturating_sub(1),
                            length: node.position.end - node.position.start,
                        }),
                    });
                    self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
                }
            }
        }
    }
    
    fn expand_preserve(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 1 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("preserve() expects exactly 1 argument, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        // The preserve function outputs its argument as-is, without expansion
        // Just convert to string representation without expanding
        let preserved_content = self.expression_to_string(&node.arguments[0]);
        self.append_to_expanded(&preserved_content, context, generate_source_map, Some(source_range));
    }
    
    fn expand_label(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 1 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("label() expects exactly 1 argument, got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            self.append_to_expanded(&self.node_to_string(&[ContentNode::BuiltinFunction(node.clone())]), context, generate_source_map, Some(source_range));
            return;
        }
        
        // Get the label name from the argument
        let label_name = self.expression_to_string(&node.arguments[0]).trim().to_string();
        
        // Check if this label already has a generated name in the current macro invocation
        let generated_label = if let Some(existing) = self.label_map.get(&label_name) {
            existing.clone()
        } else {
            // Generate a new unique label
            self.label_counter += 1;
            let new_label = format!("{}_{}", label_name, self.label_counter);
            self.label_map.insert(label_name.clone(), new_label.clone());
            new_label
        };
        
        // Output the generated label
        self.append_to_expanded(&generated_label, context, generate_source_map, Some(source_range));
    }
    
    fn expand_br(&mut self, _node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        // {br} simply outputs a newline
        self.append_to_expanded("\n", context, generate_source_map, Some(source_range));
    }
    
    fn extract_array_values(&mut self, array_node: &ExpressionNode, context: &mut ExpansionContext, is_tuple_pattern: bool) -> Vec<String> {
        match array_node {
            ExpressionNode::ArrayLiteral(array_literal) => {
                array_literal.elements.iter()
                    .map(|el| self.expand_expression_to_string(el, context).trim().to_string())
                    .collect()
            }
            _ => {
                let expanded = self.expand_expression_to_string(array_node, context).trim().to_string();
                
                // Check if it's a string literal "ABC" and convert to character array
                if expanded.starts_with('"') && expanded.ends_with('"') && expanded.len() >= 2 {
                    // Convert string literal to character array with ASCII codes
                    let string_content = &expanded[1..expanded.len()-1];
                    string_content.chars()
                        .map(|ch| (ch as u8).to_string())
                        .collect()
                } else if is_tuple_pattern && expanded.starts_with('{') && expanded.ends_with('}') && !self.looks_like_array_of_arrays(&expanded) {
                    // Single tuple to destructure
                    vec![expanded]
                } else if self.looks_like_array_of_arrays(&expanded) {
                    self.parse_array_elements(&expanded)
                } else if expanded.starts_with('{') && expanded.ends_with('}') {
                    let inner = &expanded[1..expanded.len()-1];
                    let values = self.parse_array_elements(inner);
                    
                    if is_tuple_pattern && values.len() == 1 && values[0].starts_with('{') && values[0].ends_with('}') {
                        values
                    } else {
                        values
                    }
                } else if expanded.contains(',') {
                    self.parse_array_elements(&expanded)
                } else {
                    vec![expanded]
                }
            }
        }
    }
    
    fn parse_tuple_elements(&self, value: &str) -> Vec<String> {
        if value.starts_with('{') && value.ends_with('}') {
            self.parse_array_elements(&value[1..value.len()-1])
        } else {
            value.split(',').map(|v| v.trim().to_string()).collect()
        }
    }
    
    fn looks_like_array_of_arrays(&self, s: &str) -> bool {
        let trimmed = s.trim();
        trimmed.contains(',') && trimmed.contains("},{")
    }
    
    fn parse_array_elements(&self, inner: &str) -> Vec<String> {
        let mut elements = Vec::new();
        let mut current = String::new();
        let mut brace_depth = 0;
        let mut in_single_quote = false;
        let mut in_double_quote = false;
        let mut escaped = false;
        
        for ch in inner.chars() {
            if escaped {
                current.push(ch);
                escaped = false;
                continue;
            }
            
            match ch {
                '\\' => {
                    escaped = true;
                    current.push(ch);
                }
                '\'' if !in_double_quote => {
                    in_single_quote = !in_single_quote;
                    current.push(ch);
                }
                '"' if !in_single_quote => {
                    in_double_quote = !in_double_quote;
                    current.push(ch);
                }
                '{' if !in_single_quote && !in_double_quote => {
                    brace_depth += 1;
                    current.push(ch);
                }
                '}' if !in_single_quote && !in_double_quote => {
                    brace_depth -= 1;
                    current.push(ch);
                }
                ',' if brace_depth == 0 && !in_single_quote && !in_double_quote => {
                    if !current.trim().is_empty() {
                        elements.push(current.trim().to_string());
                    }
                    current.clear();
                }
                _ => current.push(ch)
            }
        }
        
        if !current.trim().is_empty() {
            elements.push(current.trim().to_string());
        }
        
        elements
    }
    
    fn expand_proc(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 1 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("proc() expects exactly 1 argument (the body), got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        // Save current state
        let saved_in_proc = self.in_proc;
        let saved_local_counter = self.local_counter;
        let saved_local_map = self.local_map.clone();
        
        // Start fresh for this proc
        self.in_proc = true;
        self.local_counter = 0;
        self.local_map.clear();
        
        // First pass: collect locals only (don't expand them yet)
        let body_expr = &node.arguments[0];
        
        eprintln!("DEBUG expand_proc: body_expr type = {:?}", std::mem::discriminant(body_expr));
        
        // We need to walk the AST to find and process {local} declarations
        self.collect_locals_from_expression(body_expr);
        
        eprintln!("DEBUG expand_proc: Found {} locals", self.local_counter);
        eprintln!("DEBUG expand_proc: local_map = {:?}", self.local_map);
        
        // Now we know the total locals needed
        let total_locals = self.local_counter;
        
        // Second pass: expand the body with local substitutions
        let mut final_context = ExpansionContext {
            source_map_builder: SourceMapBuilder::new(),
            current_source_position: context.current_source_position.clone(),
            expansion_depth: context.expansion_depth,
            macro_call_stack: context.macro_call_stack.clone(),
            expanded_lines: vec![String::new()],
            current_expanded_line: 1,
            current_expanded_column: 1,
        };
        
        // Process the body with locals now available for substitution
        let body_with_locals = if let ExpressionNode::ExpressionList(list) = body_expr {
            // Process each content node in the list
            for content in &list.expressions {
                // Special handling for text nodes that contain {local ...}
                if let ContentNode::Text(text) = content {
                    let mut text_value = text.value.clone();
                    // Remove {local ...} declarations from the text (both formats)
                    loop {
                        let start_opt = text_value.find("{local ").or_else(|| text_value.find("{local("));
                        if let Some(start) = start_opt {
                            let is_paren = text_value[start..].starts_with("{local(");
                            let close_char = if is_paren { ')' } else { '}' };
                            let search_start = start + 7; // Skip "{local(" or "{local "
                            if let Some(end_offset) = text_value[search_start..].find(close_char) {
                                let end = search_start + end_offset + 1; // Include closing char
                                if is_paren && end < text_value.len() && text_value.chars().nth(end) == Some('}') {
                                    text_value.replace_range(start..end + 1, ""); // Include the closing }
                                } else if !is_paren {
                                    text_value.replace_range(start..end, "");
                                }
                            } else {
                                break;
                            }
                        } else {
                            break;
                        }
                    }
                    // Expand the remaining text with local references
                    if !text_value.trim().is_empty() {
                        let expanded = self.expand_local_references(&text_value);
                        self.append_to_expanded(&expanded, &mut final_context, false, None);
                    }
                } else {
                    self.expand_content(content, &mut final_context, false);
                }
            }
            final_context.expanded_lines.join("\n")
        } else {
            // Fallback for other expression types
            self.expand_expression(body_expr, &mut final_context, false);
            final_context.expanded_lines.join("\n")
        };
        
        // Replace {locals_len} with the actual count
        let mut final_body = body_with_locals;
        final_body = final_body.replace("{locals_len}", &total_locals.to_string());
        
        // Output the expanded body with all substitutions
        self.append_to_expanded(&final_body, context, generate_source_map, Some(source_range));
        
        // Restore previous state
        self.in_proc = saved_in_proc;
        self.local_counter = saved_local_counter;
        self.local_map = saved_local_map;
    }
    
    fn expand_local(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, _generate_source_map: bool, _source_range: PositionRange) {
        if node.arguments.is_empty() || node.arguments.len() > 2 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("local() expects 1 or 2 arguments (name, optional size), got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        // Get the variable name
        let var_name = self.expression_to_string(&node.arguments[0]).trim().to_string();
        
        // Check if it starts with %
        if !var_name.starts_with('%') {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("Local variable names must start with '%', got '{}'", var_name),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        // If we're in a proc and this local already exists in the map, it means we're in the
        // second pass (expansion phase) and should just skip this - locals were already collected
        if self.in_proc && self.local_map.contains_key(&var_name) {
            // Local declarations don't output anything
            return;
        }
        
        // Get the size (default to 1)
        let size = if node.arguments.len() == 2 {
            let size_expr = self.expand_expression_to_string(&node.arguments[1], context);
            let trimmed = size_expr.trim();
            
            if let Ok(s) = trimmed.parse::<usize>() {
                s
            } else {
                self.errors.push(MacroExpansionError {
                    error_type: MacroExpansionErrorType::SyntaxError,
                    message: format!("Invalid size for local variable: {}", size_expr),
                    location: Some(SourceLocation {
                        line: node.position.line.saturating_sub(1),
                        column: node.position.column.saturating_sub(1),
                        length: node.position.end - node.position.start,
                    }),
                });
                return;
            }
        } else {
            1
        };
        
        // Add to local map
        self.local_map.insert(var_name.clone(), LocalInfo {
            offset: self.local_counter,
            size,
        });
        self.local_counter += size;
        
        // Local declarations don't output anything
    }
    
    fn collect_locals_from_expression(&mut self, expr: &ExpressionNode) {
        match expr {
            ExpressionNode::ExpressionList(list) => {
                // The body is an ExpressionList containing ContentNodes
                for content in &list.expressions {
                    self.collect_locals_from_content(content);
                }
            }
            ExpressionNode::BuiltinFunction(builtin) => {
                if builtin.name == BuiltinFunction::Local {
                    // Process the local declaration
                    if !builtin.arguments.is_empty() && builtin.arguments.len() <= 2 {
                        let var_name = self.expression_to_string(&builtin.arguments[0]).trim().to_string();
                        
                        if var_name.starts_with('%') && !self.local_map.contains_key(&var_name) {
                            let size = if builtin.arguments.len() == 2 {
                                let size_str = self.expression_to_string(&builtin.arguments[1]).trim().to_string();
                                size_str.parse::<usize>().unwrap_or(1)
                            } else {
                                1
                            };
                            
                            self.local_map.insert(var_name.clone(), LocalInfo {
                                offset: self.local_counter,
                                size,
                            });
                            self.local_counter += size;
                        }
                    }
                } else {
                    // Recursively check arguments of other builtins
                    for arg in &builtin.arguments {
                        self.collect_locals_from_expression(arg);
                    }
                }
            }
            ExpressionNode::MacroInvocation(macro_inv) => {
                // Check arguments of macro invocations
                if let Some(args) = &macro_inv.arguments {
                    for arg in args {
                        self.collect_locals_from_expression(arg);
                    }
                }
            }
            _ => {}
        }
    }
    
    fn collect_locals_from_content(&mut self, content: &ContentNode) {
        match content {
            ContentNode::Text(text) => {
                eprintln!("DEBUG collect_locals_from_content: Text = '{}'", text.value);
                // Check if the text contains {local ...} patterns (can be multiple)
                // This handles cases where {local} comes through macro expansion as text
                let text_str = text.value.clone();
                let mut search_start = 0;
                
                // Keep looking for {local} patterns until we've found them all
                loop {
                    // Match both "{local " and "{local(" formats
                    let remaining = &text_str[search_start..];
                    let start_opt = remaining.find("{local ").or_else(|| remaining.find("{local("));
                    
                    if let Some(start_in_remaining) = start_opt {
                        let start = search_start + start_in_remaining;
                        let is_paren = text_str[start..].starts_with("{local(");
                        let skip_len = 7; // Skip "{local " or "{local("
                        let close_char = if is_paren { ')' } else { '}' };
                        
                        if let Some(end) = text_str[start + skip_len..].find(close_char) {
                            let local_content = &text_str[start + skip_len..start + skip_len + end];
                            eprintln!("DEBUG: Found local content: '{}'", local_content);
                            // Handle both space-separated and comma-separated formats
                            let parts: Vec<&str> = if local_content.contains(',') {
                                local_content.split(',').map(|s| s.trim()).collect()
                            } else {
                                local_content.trim().split_whitespace().collect()
                            };
                            
                            if !parts.is_empty() {
                                let var_name = parts[0].to_string();
                                eprintln!("DEBUG: Processing local var: '{}'", var_name);
                                if var_name.starts_with('%') && !self.local_map.contains_key(&var_name) {
                                    let size = if parts.len() > 1 {
                                        parts[1].parse::<usize>().unwrap_or(1)
                                    } else {
                                        1
                                    };
                                    
                                    eprintln!("DEBUG: Adding local '{}' at offset {} with size {}", var_name, self.local_counter, size);
                                    self.local_map.insert(var_name.clone(), LocalInfo {
                                        offset: self.local_counter,
                                        size,
                                    });
                                    self.local_counter += size;
                                }
                            }
                            
                            // Move past this {local} and continue searching
                            search_start = start + skip_len + end + 1;
                            if is_paren && search_start < text_str.len() && text_str.chars().nth(search_start) == Some('}') {
                                search_start += 1;
                            }
                        } else {
                            break; // No closing bracket found
                        }
                    } else {
                        break; // No more {local} patterns found
                    }
                }
            }
            ContentNode::BuiltinFunction(builtin) => {
                if builtin.name == BuiltinFunction::Local {
                    // Process the local declaration
                    if !builtin.arguments.is_empty() && builtin.arguments.len() <= 2 {
                        let var_name = self.expression_to_string(&builtin.arguments[0]).trim().to_string();
                        
                        if var_name.starts_with('%') && !self.local_map.contains_key(&var_name) {
                            let size = if builtin.arguments.len() == 2 {
                                let size_str = self.expression_to_string(&builtin.arguments[1]).trim().to_string();
                                size_str.parse::<usize>().unwrap_or(1)
                            } else {
                                1
                            };
                            
                            self.local_map.insert(var_name.clone(), LocalInfo {
                                offset: self.local_counter,
                                size,
                            });
                            self.local_counter += size;
                        }
                    }
                } else {
                    // Recursively check arguments of other builtins  
                    for arg in &builtin.arguments {
                        self.collect_locals_from_expression(arg);
                    }
                }
            }
            ContentNode::MacroInvocation(macro_inv) => {
                // Check arguments of macro invocations
                if let Some(args) = &macro_inv.arguments {
                    for arg in args {
                        self.collect_locals_from_expression(arg);
                    }
                }
            }
            _ => {}
        }
    }
    
    fn expand_len(&mut self, node: &BuiltinFunctionNode, context: &mut ExpansionContext, generate_source_map: bool, source_range: PositionRange) {
        if node.arguments.len() != 1 {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("len() expects exactly 1 argument (variable name), got {}", node.arguments.len()),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        let var_name = self.expression_to_string(&node.arguments[0]).trim().to_string();
        
        if !var_name.starts_with('%') {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::SyntaxError,
                message: format!("Variable name must start with '%', got '{}'", var_name),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
            return;
        }
        
        if let Some(info) = self.local_map.get(&var_name) {
            self.append_to_expanded(&info.size.to_string(), context, generate_source_map, Some(source_range));
        } else {
            self.errors.push(MacroExpansionError {
                error_type: MacroExpansionErrorType::Undefined,
                message: format!("Undefined local variable: {}", var_name),
                location: Some(SourceLocation {
                    line: node.position.line.saturating_sub(1),
                    column: node.position.column.saturating_sub(1),
                    length: node.position.end - node.position.start,
                }),
            });
        }
    }
    
    fn _expand_expression_to_string_with_locals(&mut self, expr: &ExpressionNode, context: &ExpansionContext) -> String {
        let mut temp_context = ExpansionContext {
            source_map_builder: SourceMapBuilder::new(),
            current_source_position: context.current_source_position.clone(),
            expansion_depth: context.expansion_depth,
            macro_call_stack: context.macro_call_stack.clone(),
            expanded_lines: vec![String::new()],
            current_expanded_line: 1,
            current_expanded_column: 1,
        };
        
        self._expand_expression_with_locals(expr, &mut temp_context, false);
        temp_context.expanded_lines.join("\n").trim().to_string()
    }
    
    fn _expand_expression_with_locals(&mut self, expr: &ExpressionNode, context: &mut ExpansionContext, generate_source_map: bool) {
        match expr {
            ExpressionNode::Text(text) => {
                // Check if it's a local variable reference
                let expanded_text = self.expand_local_references(&text.value);
                let source_range = PositionRange {
                    start: Position {
                        line: expr.position().line,
                        column: expr.position().column,
                        offset: Some(expr.position().start),
                    },
                    end: Position {
                        line: expr.position().line,
                        column: expr.position().column + (expr.position().end - expr.position().start),
                        offset: Some(expr.position().end),
                    },
                };
                self.append_to_expanded(&expanded_text, context, generate_source_map, Some(source_range));
            }
            _ => {
                // For other expression types, use the normal expansion
                self.expand_expression(expr, context, generate_source_map);
            }
        }
    }
    
}