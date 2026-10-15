//! Parser for the lossless IR text format.

use super::IrTextError;
use crate::ir::{
    AsmOperandIR, BasicBlock, FatPointer, Function, GlobalVariable, Instruction, IrBinaryOp,
    IrType, IrUnaryOp, Linkage, Module, Value,
};
use crate::BankTag;
use rcc_common::SourceLocation;

pub fn parse_module(text: &str) -> Result<Module, IrTextError> {
    let tokens = lex(text)?;
    let mut parser = Parser { tokens, pos: 0 };
    parser.parse_module()
}

struct Token {
    kind: TokenKind,
    line: u32,
    column: u32,
}

#[derive(Clone, Debug, PartialEq)]
enum TokenKind {
    Ident(String),
    String(String),
    Int(u64),
    Minus,
    Arrow,
    At,
    Percent,
    Star,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    Equal,
    DotDotDot,
    Newline,
}

struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    fn parse_module(&mut self) -> Result<Module, IrTextError> {
        self.expect_keyword("module")?;
        let name = self.parse_name()?;
        self.expect(&TokenKind::LBrace, "'{'")?;
        let mut module = Module::new(name);
        let mut saw_target = false;
        loop {
            if self.peek_kind(&TokenKind::RBrace) {
                break;
            }
            if self.eof() {
                return Err(self.error("unclosed module"));
            }
            if self.eat_keyword("target") {
                if saw_target { return Err(self.error("duplicate module target")); }
                saw_target = true;
                let name = self.expect_ident()?;
                module.target = crate::target::Target::parse(&name).map_err(|e| self.error(e))?;
            } else if self.peek_kind(&TokenKind::At) {
                module.globals.push(self.parse_global()?);
            } else if self.eat_keyword("type") {
                let key = self.parse_name()?;
                self.expect(&TokenKind::Equal, "'='")?;
                let ty = self.parse_type()?;
                module.type_definitions.insert(key, ty);
            } else if self.eat_keyword("internal") {
                self.expect_keyword("function")?;
                module.internal_functions.push(self.parse_at_name()?);
            } else if self.eat_keyword("function") {
                module.functions.push(self.parse_function()?);
            } else {
                return Err(self.error("expected a type, internal function, global, or function"));
            }
        }
        self.expect(&TokenKind::RBrace, "'}'")?;
        if !self.eof() {
            return Err(self.error("unexpected tokens after module"));
        }
        Ok(module)
    }

    fn parse_global(&mut self) -> Result<GlobalVariable, IrTextError> {
        let name = self.parse_at_name()?;
        self.expect(&TokenKind::Equal, "'='")?;
        let linkage = self.parse_linkage()?;
        let is_constant = self.eat_keyword("constant");
        let var_type = self.parse_type()?;
        let initializer = if self.eat(&TokenKind::Equal) {
            Some(self.parse_value()?)
        } else {
            None
        };
        let symbol_id = self.parse_optional_symbol()?;
        Ok(GlobalVariable {
            name,
            var_type,
            is_constant,
            initializer,
            linkage,
            symbol_id,
        })
    }

    fn parse_function(&mut self) -> Result<Function, IrTextError> {
        let is_external = self.eat_keyword("external");
        let is_vararg = self.eat_keyword("vararg");
        let return_type = self.parse_type()?;
        let name = self.parse_at_name()?;
        self.expect(&TokenKind::LParen, "'('")?;
        let mut parameters = Vec::new();
        if !self.peek_kind(&TokenKind::RParen) {
            loop {
                let param_type = self.parse_type()?;
                self.expect(&TokenKind::Percent, "'%'")?;
                let param_id = self.parse_u32()?;
                parameters.push((param_id, param_type));
                if !self.eat(&TokenKind::Comma) {
                    break;
                }
                if self.peek_kind(&TokenKind::RParen) {
                    return Err(self.error("trailing comma in parameter list"));
                }
            }
        }
        self.expect(&TokenKind::RParen, "')'")?;
        let symbol_id = self.parse_optional_symbol()?;
        let source_location = if self.eat_keyword("loc") {
            let filename = self.parse_name()?;
            let line = self.parse_u32()?;
            let column = self.parse_u32()?;
            Some(SourceLocation::new(&filename, line, column))
        } else {
            None
        };
        self.expect(&TokenKind::LBrace, "'{'")?;
        let blocks = self.parse_blocks()?;
        self.expect(&TokenKind::RBrace, "'}'")?;
        Ok(Function {
            name,
            return_type,
            parameters,
            blocks,
            is_external,
            is_vararg,
            symbol_id,
            source_location,
        })
    }

    fn parse_blocks(&mut self) -> Result<Vec<BasicBlock>, IrTextError> {
        let mut blocks = Vec::new();
        loop {
            if self.peek_kind(&TokenKind::RBrace) || self.eof() {
                break;
            }
            self.expect_keyword("block")?;
            let id = self.parse_u32()?;
            let is_loop_condition = self.eat_keyword("loop");
            let predecessors = if self.eat_keyword("preds") {
                self.parse_u32_list()?
            } else {
                Vec::new()
            };
            let successors = if self.eat_keyword("succs") {
                self.parse_u32_list()?
            } else {
                Vec::new()
            };
            self.expect(&TokenKind::Colon, "':'")?;
            let mut instructions = Vec::new();
            loop {
                if self.peek_kind(&TokenKind::RBrace) || self.peek_keyword("block") || self.eof() {
                    break;
                }
                let line = self.take_line()?;
                let mut line_parser = Parser { tokens: line, pos: 0 };
                instructions.push(line_parser.parse_instruction()?);
            }
            blocks.push(BasicBlock {
                id,
                instructions,
                predecessors,
                successors,
                is_loop_condition,
            });
        }
        Ok(blocks)
    }

    fn parse_instruction(&mut self) -> Result<Instruction, IrTextError> {
        let (result, result_at) = if self.peek_kind(&TokenKind::Percent) {
            let at = self.location();
            self.bump();
            let id = self.parse_u32()?;
            self.expect(&TokenKind::Equal, "'='")?;
            (Some(id), Some(at))
        } else {
            (None, None)
        };
        let opcode_at = self.location();
        let opcode = self.expect_ident()?;
        let inst = match opcode.as_str() {
            "load" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let volatile = self.eat_keyword("volatile");
                let result_type = self.parse_type()?;
                self.expect(&TokenKind::Comma, "','")?;
                let ptr = self.parse_value()?;
                Instruction::Load { result, ptr, result_type, volatile }
            }
            "store" => {
                self.forbid_result(result, result_at, &opcode)?;
                let volatile = self.eat_keyword("volatile");
                let value_type = self.parse_type()?;
                let value = self.parse_value()?;
                self.expect(&TokenKind::Comma, "','")?;
                let ptr = self.parse_value()?;
                Instruction::Store { value, ptr, value_type, volatile }
            }
            "getelementptr" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let result_type = self.parse_type()?;
                self.expect(&TokenKind::Comma, "','")?;
                let source_element_type = self.parse_type()?;
                let ptr = self.parse_value()?;
                let mut indices = Vec::new();
                while self.eat(&TokenKind::Comma) {
                    indices.push(self.parse_value()?);
                }
                let is_struct_field = self.eat_keyword("field");
                Instruction::GetElementPtr {
                    result,
                    ptr,
                    indices,
                    result_type,
                    source_element_type,
                    is_struct_field,
                }
            }
            "alloca" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let alloc_type = self.parse_type()?;
                let count = if self.eat(&TokenKind::Comma) {
                    Some(self.parse_value()?)
                } else {
                    None
                };
                self.expect(&TokenKind::Arrow, "'->'")?;
                let result_type = self.parse_type()?;
                Instruction::Alloca { result, alloc_type, count, result_type }
            }
            "call" => {
                let result_type = self.parse_type()?;
                let function = self.parse_value()?;
                self.expect(&TokenKind::LParen, "'('")?;
                let args = self.parse_value_list()?;
                self.expect(&TokenKind::RParen, "')'")?;
                let stack_args_from = if self.eat_keyword("stack") {
                    Some(self.parse_usize()?)
                } else {
                    None
                };
                Instruction::Call { result, function, args, result_type, stack_args_from }
            }
            "intrinsic" => {
                let result_type = self.parse_type()?;
                let intrinsic = self.parse_at_name()?;
                self.expect(&TokenKind::LParen, "'('")?;
                let args = self.parse_value_list()?;
                self.expect(&TokenKind::RParen, "')'")?;
                Instruction::Intrinsic { result, intrinsic, args, result_type }
            }
            "ret" => {
                self.forbid_result(result, result_at, &opcode)?;
                if self.eat_keyword("void") {
                    Instruction::Return(None)
                } else {
                    Instruction::Return(Some(self.parse_value()?))
                }
            }
            "br" => {
                self.forbid_result(result, result_at, &opcode)?;
                if self.eat_keyword("label") {
                    Instruction::Branch(self.parse_u32()?)
                } else {
                    let condition = self.parse_value()?;
                    self.expect(&TokenKind::Comma, "','")?;
                    self.expect_keyword("label")?;
                    let true_label = self.parse_u32()?;
                    self.expect(&TokenKind::Comma, "','")?;
                    self.expect_keyword("label")?;
                    let false_label = self.parse_u32()?;
                    Instruction::BranchCond { condition, true_label, false_label }
                }
            }
            "phi" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let result_type = self.parse_type()?;
                let mut incoming = Vec::new();
                while self.eat(&TokenKind::LBracket) {
                    let value = self.parse_value()?;
                    self.expect(&TokenKind::Comma, "','")?;
                    self.expect_keyword("label")?;
                    let label = self.parse_u32()?;
                    self.expect(&TokenKind::RBracket, "']'")?;
                    incoming.push((value, label));
                    if self.peek_kind(&TokenKind::Comma) {
                        let saved = self.pos;
                        self.bump();
                        if !self.peek_kind(&TokenKind::LBracket) {
                            self.pos = saved;
                            break;
                        }
                    } else {
                        break;
                    }
                }
                Instruction::Phi { result, incoming, result_type }
            }
            "cast" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let value = self.parse_value()?;
                self.expect_keyword("to")?;
                let target_type = self.parse_type()?;
                Instruction::Cast { result, value, target_type }
            }
            "select" => {
                let result = self.require_result(result, result_at, &opcode)?;
                let result_type = self.parse_type()?;
                let condition = self.parse_value()?;
                self.expect(&TokenKind::Comma, "','")?;
                let true_value = self.parse_value()?;
                self.expect(&TokenKind::Comma, "','")?;
                let false_value = self.parse_value()?;
                Instruction::Select { result, condition, true_value, false_value, result_type }
            }
            "debugloc" => {
                self.forbid_result(result, result_at, &opcode)?;
                let filename = self.parse_name()?;
                let line = self.parse_u32()?;
                let column = self.parse_u32()?;
                Instruction::DebugLoc { location: SourceLocation::new(&filename, line, column) }
            }
            "asm" => {
                self.forbid_result(result, result_at, &opcode)?;
                let assembly = self.expect_string()?;
                let outputs = if self.eat_keyword("outs") {
                    self.parse_operands()?
                } else {
                    Vec::new()
                };
                let inputs = if self.eat_keyword("ins") {
                    self.parse_operands()?
                } else {
                    Vec::new()
                };
                let clobbers = if self.eat_keyword("clobbers") {
                    self.parse_string_list()?
                } else {
                    Vec::new()
                };
                Instruction::InlineAsm { assembly, outputs, inputs, clobbers }
            }
            "comment" => {
                self.forbid_result(result, result_at, &opcode)?;
                Instruction::Comment(self.expect_string()?)
            }
            other => {
                if let Some(op) = parse_binary_op(other) {
                    let result = self.require_result(result, result_at, other)?;
                    let result_type = self.parse_type()?;
                    let lhs = self.parse_value()?;
                    self.expect(&TokenKind::Comma, "','")?;
                    let rhs = self.parse_value()?;
                    Instruction::Binary { result, op, lhs, rhs, result_type }
                } else if let Some(op) = parse_unary_op(other) {
                    let result = self.require_result(result, result_at, other)?;
                    let result_type = self.parse_type()?;
                    let operand = self.parse_value()?;
                    Instruction::Unary { result, op, operand, result_type }
                } else {
                    return Err(IrTextError {
                        line: opcode_at.0,
                        column: opcode_at.1,
                        message: format!("unknown instruction '{other}'"),
                    });
                }
            }
        };
        self.finish()?;
        Ok(inst)
    }

    fn parse_operands(&mut self) -> Result<Vec<AsmOperandIR>, IrTextError> {
        let mut operands = Vec::new();
        if !self.peek_string() {
            return Ok(operands);
        }
        loop {
            let constraint = self.expect_string()?;
            self.expect(&TokenKind::LParen, "'('")?;
            let value = self.parse_value()?;
            self.expect(&TokenKind::RParen, "')'")?;
            let tied_to = if self.eat_keyword("tie") {
                Some(self.parse_usize()?)
            } else {
                None
            };
            operands.push(AsmOperandIR { constraint, value, tied_to });
            if !self.eat(&TokenKind::Comma) {
                break;
            }
            if !self.peek_string() {
                return Err(self.error("trailing comma in assembly operand list"));
            }
        }
        Ok(operands)
    }

    fn parse_string_list(&mut self) -> Result<Vec<String>, IrTextError> {
        let mut values = Vec::new();
        if !self.peek_string() {
            return Ok(values);
        }
        loop {
            values.push(self.expect_string()?);
            if !self.eat(&TokenKind::Comma) {
                break;
            }
            if !self.peek_string() {
                return Err(self.error("trailing comma in clobber list"));
            }
        }
        Ok(values)
    }

    fn parse_value_list(&mut self) -> Result<Vec<Value>, IrTextError> {
        let mut values = Vec::new();
        if self.peek_kind(&TokenKind::RParen) {
            return Ok(values);
        }
        loop {
            values.push(self.parse_value()?);
            if !self.eat(&TokenKind::Comma) {
                break;
            }
            if self.peek_kind(&TokenKind::RParen) {
                return Err(self.error("trailing comma in value list"));
            }
        }
        Ok(values)
    }

    fn parse_type(&mut self) -> Result<IrType, IrTextError> {
        let mut ty = self.parse_primary_type()?;
        loop {
            if self.eat(&TokenKind::Star) {
                ty = IrType::FatPtr(Box::new(ty));
            } else if self.eat(&TokenKind::LParen) {
                let mut param_types = Vec::new();
                let mut is_vararg = false;
                if !self.peek_kind(&TokenKind::RParen) {
                    loop {
                        if self.eat(&TokenKind::DotDotDot) {
                            is_vararg = true;
                            break;
                        }
                        param_types.push(self.parse_type()?);
                        if !self.eat(&TokenKind::Comma) {
                            break;
                        }
                        if self.peek_kind(&TokenKind::RParen) {
                            return Err(self.error("trailing comma in function type"));
                        }
                    }
                }
                self.expect(&TokenKind::RParen, "')'")?;
                ty = IrType::Function {
                    return_type: Box::new(ty),
                    param_types,
                    is_vararg,
                };
            } else {
                break;
            }
        }
        Ok(ty)
    }

    fn parse_primary_type(&mut self) -> Result<IrType, IrTextError> {
        if self.eat(&TokenKind::LBracket) {
            let size = self.parse_u64()?.0;
            self.expect_keyword("x")?;
            let element_type = self.parse_type()?;
            self.expect(&TokenKind::RBracket, "']'")?;
            return Ok(IrType::Array { size, element_type: Box::new(element_type) });
        }
        let packed = self.eat_keyword("packed");
        if packed || self.peek_keyword("struct") {
            self.expect_keyword("struct")?;
            let name = if self.peek_string() || self.peek_ident() {
                Some(self.parse_name()?)
            } else {
                None
            };
            self.expect(&TokenKind::LBrace, "'{'")?;
            let mut fields = Vec::new();
            if !self.peek_kind(&TokenKind::RBrace) {
                loop {
                    fields.push(self.parse_type()?);
                    if !self.eat(&TokenKind::Comma) {
                        break;
                    }
                    if self.peek_kind(&TokenKind::RBrace) {
                        return Err(self.error("trailing comma in struct type"));
                    }
                }
            }
            self.expect(&TokenKind::RBrace, "'}'")?;
            return Ok(IrType::Struct { name, fields, packed });
        }
        let (word, line, column) = self.expect_ident_at()?;
        let ty = match word.as_str() {
            "void" => IrType::Void,
            "i1" => IrType::I1,
            "i8" => IrType::I8,
            "i16" => IrType::I16,
            "i32" => IrType::I32,
            "i64" => IrType::I64,
            "f32" => IrType::F32,
            "f64" => IrType::F64,
            "label" => IrType::Label,
            other => {
                return Err(IrTextError {
                    line,
                    column,
                    message: format!("unknown type '{other}'"),
                });
            }
        };
        Ok(ty)
    }

    fn parse_value(&mut self) -> Result<Value, IrTextError> {
        if self.eat(&TokenKind::Percent) {
            return Ok(Value::Temp(self.parse_u32()?));
        }
        if self.peek_kind(&TokenKind::Minus) || self.peek_kind(&TokenKind::Int(0)) {
            return Ok(Value::Constant(self.parse_i64()?));
        }
        if self.eat(&TokenKind::LBracket) {
            let mut values = Vec::new();
            if !self.peek_kind(&TokenKind::RBracket) {
                loop {
                    values.push(self.parse_i64()?);
                    if !self.eat(&TokenKind::Comma) {
                        break;
                    }
                    if self.peek_kind(&TokenKind::RBracket) {
                        return Err(self.error("trailing comma in constant array"));
                    }
                }
            }
            self.expect(&TokenKind::RBracket, "']'")?;
            return Ok(Value::ConstantArray(values));
        }
        if self.eat(&TokenKind::LBrace) {
            let values = if self.peek_kind(&TokenKind::RBrace) {
                Vec::new()
            } else {
                let mut values = Vec::new();
                loop {
                    values.push(self.parse_value()?);
                    if !self.eat(&TokenKind::Comma) {
                        break;
                    }
                    if self.peek_kind(&TokenKind::RBrace) {
                        return Err(self.error("trailing comma in aggregate"));
                    }
                }
                values
            };
            self.expect(&TokenKind::RBrace, "'}'")?;
            return Ok(Value::ConstantAggregate(values));
        }
        let word = self.expect_ident()?;
        match word.as_str() {
            "undef" => Ok(Value::Undef),
            "global" => Ok(Value::Global(self.parse_at_name()?)),
            "func" => Ok(Value::Function(self.parse_at_name()?)),
            "fat" => {
                self.expect(&TokenKind::LParen, "'('")?;
                let addr = self.parse_value()?;
                self.expect(&TokenKind::Comma, "','")?;
                let bank = self.parse_bank()?;
                self.expect(&TokenKind::RParen, "')'")?;
                Ok(Value::FatPtr(FatPointer { addr: Box::new(addr), bank }))
            }
            "offset" => {
                self.expect(&TokenKind::LParen, "'('")?;
                let base = self.parse_value()?;
                self.expect(&TokenKind::Comma, "','")?;
                let cells = self.parse_i64()?;
                self.expect(&TokenKind::RParen, "')'")?;
                Ok(Value::AddressOffset { base: Box::new(base), cells })
            }
            other => Err(self.error(format!("unknown value '{other}'"))),
        }
    }

    fn parse_bank(&mut self) -> Result<BankTag, IrTextError> {
        let (word, line, column) = self.expect_ident_at()?;
        match word.as_str() {
            "global" => Ok(BankTag::Global),
            "stack" => Ok(BankTag::Stack),
            "unknown" => Ok(BankTag::Unknown),
            "mixed" => Ok(BankTag::Mixed),
            "null" => Ok(BankTag::Null),
            "heap" => {
                self.expect(&TokenKind::LParen, "'('")?;
                let (n, n_line, n_column) = self.parse_u64()?;
                let bank = u16::try_from(n).map_err(|_| IrTextError {
                    line: n_line,
                    column: n_column,
                    message: "heap bank does not fit in u16".to_string(),
                })?;
                self.expect(&TokenKind::RParen, "')'")?;
                Ok(BankTag::Heap(bank))
            }
            other => Err(IrTextError {
                line,
                column,
                message: format!("unknown bank '{other}'"),
            }),
        }
    }

    fn parse_linkage(&mut self) -> Result<Linkage, IrTextError> {
        let (word, line, column) = self.expect_ident_at()?;
        match word.as_str() {
            "external" => Ok(Linkage::External),
            "internal" => Ok(Linkage::Internal),
            "private" => Ok(Linkage::Private),
            "declaration" => Ok(Linkage::Declaration),
            other => Err(IrTextError {
                line,
                column,
                message: format!("unknown linkage '{other}'"),
            }),
        }
    }

    fn parse_optional_symbol(&mut self) -> Result<Option<u32>, IrTextError> {
        if self.eat_keyword("symbol") {
            Ok(Some(self.parse_u32()?))
        } else {
            Ok(None)
        }
    }

    fn parse_at_name(&mut self) -> Result<String, IrTextError> {
        self.expect(&TokenKind::At, "'@'")?;
        self.parse_name()
    }

    fn parse_name(&mut self) -> Result<String, IrTextError> {
        if self.peek_string() {
            return self.expect_string();
        }
        self.expect_ident()
    }

    fn parse_u32_list(&mut self) -> Result<Vec<u32>, IrTextError> {
        let mut ids = Vec::new();
        if !self.peek_int() {
            return Ok(ids);
        }
        loop {
            ids.push(self.parse_u32()?);
            if !self.eat(&TokenKind::Comma) {
                break;
            }
            if !self.peek_int() {
                return Err(self.error("trailing comma in label list"));
            }
        }
        Ok(ids)
    }

    fn parse_i64(&mut self) -> Result<i64, IrTextError> {
        let negative = if self.peek_kind(&TokenKind::Minus) {
            self.bump();
            true
        } else {
            false
        };
        let (magnitude, line, column) = self.parse_u64()?;
        if negative {
            if magnitude == 1u64 << 63 {
                return Ok(i64::MIN);
            }
            let value = i64::try_from(magnitude).map_err(|_| IrTextError {
                line,
                column,
                message: "integer does not fit in i64".to_string(),
            })?;
            Ok(-value)
        } else {
            i64::try_from(magnitude).map_err(|_| IrTextError {
                line,
                column,
                message: "integer does not fit in i64".to_string(),
            })
        }
    }

    fn parse_u32(&mut self) -> Result<u32, IrTextError> {
        let (n, line, column) = self.parse_u64()?;
        u32::try_from(n).map_err(|_| IrTextError {
            line,
            column,
            message: "integer does not fit in u32".to_string(),
        })
    }

    fn parse_usize(&mut self) -> Result<usize, IrTextError> {
        let (n, line, column) = self.parse_u64()?;
        usize::try_from(n).map_err(|_| IrTextError {
            line,
            column,
            message: "integer does not fit in usize".to_string(),
        })
    }

    fn parse_u64(&mut self) -> Result<(u64, u32, u32), IrTextError> {
        self.skip_noise();
        match self.tokens.get(self.pos) {
            Some(Token { kind: TokenKind::Int(n), line, column }) => {
                let located = (*n, *line, *column);
                self.pos += 1;
                Ok(located)
            }
            _ => Err(self.error("expected integer")),
        }
    }

    fn require_result(
        &mut self,
        result: Option<u32>,
        at: Option<(u32, u32)>,
        opcode: &str,
    ) -> Result<u32, IrTextError> {
        match result {
            Some(id) => Ok(id),
            None => {
                let (line, column) = at.unwrap_or_else(|| self.location());
                Err(IrTextError {
                    line,
                    column,
                    message: format!("{opcode} requires a result temporary"),
                })
            }
        }
    }

    fn forbid_result(
        &mut self,
        result: Option<u32>,
        at: Option<(u32, u32)>,
        opcode: &str,
    ) -> Result<(), IrTextError> {
        if result.is_some() {
            let (line, column) = at.unwrap_or_else(|| self.location());
            Err(IrTextError {
                line,
                column,
                message: format!("{opcode} does not produce a result"),
            })
        } else {
            Ok(())
        }
    }

    fn take_line(&mut self) -> Result<Vec<Token>, IrTextError> {
        self.skip_noise();
        if self.eof() {
            return Err(self.error("expected an instruction"));
        }
        let start = self.pos;
        let mut depth = 0i32;
        while self.pos < self.tokens.len() {
            match &self.tokens[self.pos].kind {
                TokenKind::Newline if depth == 0 => {
                    self.pos += 1;
                    break;
                }
                TokenKind::RBrace if depth == 0 => break,
                TokenKind::LParen | TokenKind::LBrace | TokenKind::LBracket => {
                    depth += 1;
                    self.pos += 1;
                }
                TokenKind::RParen | TokenKind::RBrace | TokenKind::RBracket => {
                    depth -= 1;
                    if depth < 0 {
                        return Err(self.error("unbalanced delimiter"));
                    }
                    self.pos += 1;
                }
                _ => self.pos += 1,
            }
        }
        if depth != 0 {
            return Err(self.error("unclosed delimiter in instruction"));
        }
        let line = self.tokens[start..self.pos]
            .iter()
            .filter(|token| token.kind != TokenKind::Newline)
            .map(|token| Token {
                kind: token.kind.clone(),
                line: token.line,
                column: token.column,
            })
            .collect::<Vec<_>>();
        if line.is_empty() {
            return Err(self.error("expected an instruction"));
        }
        Ok(line)
    }

    fn finish(&mut self) -> Result<(), IrTextError> {
        if self.eof() {
            Ok(())
        } else {
            Err(self.error("unexpected tokens at end of instruction"))
        }
    }

    fn expect_keyword(&mut self, keyword: &str) -> Result<(), IrTextError> {
        if self.eat_keyword(keyword) {
            Ok(())
        } else {
            Err(self.error(format!("expected '{keyword}'")))
        }
    }

    fn expect_ident(&mut self) -> Result<String, IrTextError> {
        Ok(self.expect_ident_at()?.0)
    }

    fn expect_ident_at(&mut self) -> Result<(String, u32, u32), IrTextError> {
        self.skip_noise();
        match self.tokens.get(self.pos) {
            Some(Token { kind: TokenKind::Ident(text), line, column }) => {
                let found = (text.clone(), *line, *column);
                self.pos += 1;
                Ok(found)
            }
            _ => Err(self.error("expected a name")),
        }
    }

    fn expect_string(&mut self) -> Result<String, IrTextError> {
        self.skip_noise();
        match self.tokens.get(self.pos) {
            Some(Token { kind: TokenKind::String(text), .. }) => {
                let found = text.clone();
                self.pos += 1;
                Ok(found)
            }
            _ => Err(self.error("expected a string")),
        }
    }

    fn expect(&mut self, kind: &TokenKind, what: &str) -> Result<(), IrTextError> {
        if self.eat(kind) {
            Ok(())
        } else {
            Err(self.error(format!("expected {what}")))
        }
    }

    fn eat_keyword(&mut self, keyword: &str) -> bool {
        if self.peek_keyword(keyword) {
            self.bump();
            true
        } else {
            false
        }
    }

    fn eat(&mut self, kind: &TokenKind) -> bool {
        if self.peek_kind(kind) {
            self.bump();
            true
        } else {
            false
        }
    }

    fn peek_keyword(&mut self, keyword: &str) -> bool {
        self.skip_noise();
        matches!(self.tokens.get(self.pos).map(|token| &token.kind), Some(TokenKind::Ident(text)) if text == keyword)
    }

    fn peek_ident(&mut self) -> bool {
        self.skip_noise();
        matches!(self.tokens.get(self.pos).map(|token| &token.kind), Some(TokenKind::Ident(_)))
    }

    fn peek_string(&mut self) -> bool {
        self.skip_noise();
        matches!(self.tokens.get(self.pos).map(|token| &token.kind), Some(TokenKind::String(_)))
    }

    fn peek_int(&mut self) -> bool {
        self.skip_noise();
        matches!(self.tokens.get(self.pos).map(|token| &token.kind), Some(TokenKind::Int(_)))
    }

    fn peek_kind(&mut self, kind: &TokenKind) -> bool {
        self.skip_noise();
        match (self.tokens.get(self.pos).map(|token| &token.kind), kind) {
            (Some(TokenKind::Int(_)), TokenKind::Int(_)) => true,
            (Some(found), expected) => found == expected,
            _ => false,
        }
    }

    fn bump(&mut self) {
        self.skip_noise();
        if self.pos < self.tokens.len() {
            self.pos += 1;
        }
    }

    fn skip_noise(&mut self) {
        while matches!(self.tokens.get(self.pos).map(|token| &token.kind), Some(TokenKind::Newline)) {
            self.pos += 1;
        }
    }

    fn eof(&mut self) -> bool {
        self.skip_noise();
        self.pos >= self.tokens.len()
    }

    fn location(&mut self) -> (u32, u32) {
        self.skip_noise();
        if let Some(token) = self.tokens.get(self.pos) {
            (token.line, token.column)
        } else if let Some(token) = self.tokens.last() {
            (token.line, token.column)
        } else {
            (1, 1)
        }
    }

    fn error(&mut self, message: impl Into<String>) -> IrTextError {
        let (line, column) = self.location();
        IrTextError { line, column, message: message.into() }
    }
}

fn parse_binary_op(word: &str) -> Option<IrBinaryOp> {
    Some(match word {
        "add" => IrBinaryOp::Add,
        "sub" => IrBinaryOp::Sub,
        "mul" => IrBinaryOp::Mul,
        "sdiv" => IrBinaryOp::SDiv,
        "udiv" => IrBinaryOp::UDiv,
        "srem" => IrBinaryOp::SRem,
        "urem" => IrBinaryOp::URem,
        "and" => IrBinaryOp::And,
        "or" => IrBinaryOp::Or,
        "xor" => IrBinaryOp::Xor,
        "shl" => IrBinaryOp::Shl,
        "lshr" => IrBinaryOp::LShr,
        "ashr" => IrBinaryOp::AShr,
        "eq" => IrBinaryOp::Eq,
        "ne" => IrBinaryOp::Ne,
        "slt" => IrBinaryOp::Slt,
        "sle" => IrBinaryOp::Sle,
        "sgt" => IrBinaryOp::Sgt,
        "sge" => IrBinaryOp::Sge,
        "ult" => IrBinaryOp::Ult,
        "ule" => IrBinaryOp::Ule,
        "ugt" => IrBinaryOp::Ugt,
        "uge" => IrBinaryOp::Uge,
        _ => return None,
    })
}

fn parse_unary_op(word: &str) -> Option<IrUnaryOp> {
    Some(match word {
        "not" => IrUnaryOp::Not,
        "neg" => IrUnaryOp::Neg,
        "zext" => IrUnaryOp::ZExt,
        "sext" => IrUnaryOp::SExt,
        "trunc" => IrUnaryOp::Trunc,
        "ptrtolinear" => IrUnaryOp::PtrToLinear,
        "ptrtoint" => IrUnaryOp::PtrToInt,
        "inttoptr" => IrUnaryOp::IntToPtr,
        _ => return None,
    })
}

fn lex(input: &str) -> Result<Vec<Token>, IrTextError> {
    let mut tokens = Vec::new();
    let mut chars = input.chars().peekable();
    let mut line = 1u32;
    let mut column = 1u32;

    while let Some(&ch) = chars.peek() {
        let start_line = line;
        let start_column = column;
        if ch == ' ' || ch == '\t' || ch == '\r' {
            chars.next();
            column += 1;
            continue;
        }
        if ch == '\n' {
            chars.next();
            tokens.push(Token { kind: TokenKind::Newline, line, column });
            line += 1;
            column = 1;
            continue;
        }
        if ch == ';' {
            chars.next();
            column += 1;
            while let Some(&next) = chars.peek() {
                if next == '\n' {
                    break;
                }
                chars.next();
                column += 1;
            }
            continue;
        }
        if ch == '"' {
            chars.next();
            column += 1;
            let mut text = String::new();
            loop {
                let Some(next) = chars.next() else {
                    return Err(IrTextError {
                        line: start_line,
                        column: start_column,
                        message: "unterminated string".to_string(),
                    });
                };
                if next == '\n' {
                    line += 1;
                    column = 1;
                } else {
                    column += 1;
                }
                if next == '"' {
                    break;
                }
                if next != '\\' {
                    text.push(next);
                    continue;
                }
                let Some(escaped) = chars.next() else {
                    return Err(IrTextError {
                        line,
                        column,
                        message: "unterminated string escape".to_string(),
                    });
                };
                if escaped == '\n' {
                    line += 1;
                    column = 1;
                } else {
                    column += 1;
                }
                match escaped {
                    '\\' => text.push('\\'),
                    '"' => text.push('"'),
                    'n' => text.push('\n'),
                    'r' => text.push('\r'),
                    't' => text.push('\t'),
                    'u' => {
                        if chars.next() != Some('{') {
                            return Err(IrTextError {
                                line,
                                column,
                                message: "expected '{' after \\u".to_string(),
                            });
                        }
                        column += 1;
                        let mut hex = String::new();
                        loop {
                            let Some(digit) = chars.next() else {
                                return Err(IrTextError {
                                    line,
                                    column,
                                    message: "unterminated unicode escape".to_string(),
                                });
                            };
                            column += 1;
                            if digit == '}' {
                                break;
                            }
                            hex.push(digit);
                        }
                        let code = u32::from_str_radix(&hex, 16).map_err(|_| IrTextError {
                            line,
                            column,
                            message: format!("invalid unicode escape '\\u{{{hex}}}'"),
                        })?;
                        let ch = char::from_u32(code).ok_or_else(|| IrTextError {
                            line,
                            column,
                            message: format!("invalid unicode scalar '{code:x}'"),
                        })?;
                        text.push(ch);
                    }
                    other => {
                        return Err(IrTextError {
                            line,
                            column,
                            message: format!("unknown string escape '\\{other}'"),
                        });
                    }
                }
            }
            tokens.push(Token { kind: TokenKind::String(text), line: start_line, column: start_column });
            continue;
        }
        if ch.is_ascii_digit() {
            let mut number = 0u64;
            while let Some(digit) = chars.peek().and_then(|c| c.to_digit(10)) {
                chars.next();
                column += 1;
                number = number.checked_mul(10).and_then(|n| n.checked_add(u64::from(digit))).ok_or_else(|| {
                    IrTextError {
                        line: start_line,
                        column: start_column,
                        message: "integer does not fit in u64".to_string(),
                    }
                })?;
            }
            tokens.push(Token { kind: TokenKind::Int(number), line: start_line, column: start_column });
            continue;
        }
        if ch.is_ascii_alphabetic() || ch == '_' || ch == '$' {
            let mut text = String::new();
            while let Some(&next) = chars.peek() {
                if next.is_ascii_alphanumeric() || next == '_' || next == '.' || next == '$' {
                    text.push(next);
                    chars.next();
                    column += 1;
                } else {
                    break;
                }
            }
            tokens.push(Token { kind: TokenKind::Ident(text), line: start_line, column: start_column });
            continue;
        }
        chars.next();
        column += 1;
        let kind = match ch {
            '@' => TokenKind::At,
            '%' => TokenKind::Percent,
            '*' => TokenKind::Star,
            '(' => TokenKind::LParen,
            ')' => TokenKind::RParen,
            '{' => TokenKind::LBrace,
            '}' => TokenKind::RBrace,
            '[' => TokenKind::LBracket,
            ']' => TokenKind::RBracket,
            ',' => TokenKind::Comma,
            ':' => TokenKind::Colon,
            '=' => TokenKind::Equal,
            '-' => {
                if chars.peek() == Some(&'>') {
                    chars.next();
                    column += 1;
                    TokenKind::Arrow
                } else {
                    TokenKind::Minus
                }
            }
            '.' => {
                if chars.peek() == Some(&'.') {
                    let mut look = chars.clone();
                    look.next();
                    if look.peek() == Some(&'.') {
                        chars.next();
                        chars.next();
                        column += 2;
                        TokenKind::DotDotDot
                    } else {
                        return Err(IrTextError {
                            line: start_line,
                            column: start_column,
                            message: "expected '...'".to_string(),
                        });
                    }
                } else {
                    return Err(IrTextError {
                        line: start_line,
                        column: start_column,
                        message: "unexpected '.'".to_string(),
                    });
                }
            }
            other => {
                return Err(IrTextError {
                    line: start_line,
                    column: start_column,
                    message: format!("unexpected character '{other}'"),
                });
            }
        };
        tokens.push(Token { kind, line: start_line, column: start_column });
    }
    Ok(tokens)
}
