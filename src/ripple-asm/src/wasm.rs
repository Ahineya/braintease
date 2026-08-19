#[cfg(feature = "wasm")]
use wasm_bindgen::prelude::*;
use serde_wasm_bindgen::{to_value, from_value};
use crate::{RippleAssembler, AssemblerOptions, MacroFormatter, Linker};

#[cfg(feature = "wasm")]
#[wasm_bindgen]
pub struct WasmAssembler {
    assembler: RippleAssembler,
}

#[cfg(feature = "wasm")]
#[wasm_bindgen]
impl WasmAssembler {
    #[wasm_bindgen(constructor)]
    pub fn new() -> Self {
        Self {
            assembler: RippleAssembler::new(AssemblerOptions::default()),
        }
    }

    #[wasm_bindgen(js_name = "newWithOptions")]
    pub fn new_with_options(
        case_insensitive: bool,
        bank_size: u16,
        max_immediate: u32,
        memory_offset: u16,
    ) -> Self {
        let options = AssemblerOptions {
            case_insensitive,
            start_bank: 0,
            bank_size,
            max_immediate,
            memory_offset,
        };
        
        Self {
            assembler: RippleAssembler::new(options),
        }
    }

    #[wasm_bindgen(js_name = "getAvailableMnemonics")]
    pub fn get_available_mnemonics(&self) -> Vec<String> {
        use crate::types::{Opcode, Register};
        use crate::virtual_instructions::VirtualInstructionRegistry;
        
        let mut mnemonics = vec![];
        
        // Add all hardware instructions from Opcode::all()
        for opcode in Opcode::all() {
            mnemonics.push(opcode.to_string());
        }
        
        // HALT is a first-class opcode listed by Opcode::all()
        let registry = VirtualInstructionRegistry::new();
        for name in registry.get_all_names() {
            mnemonics.push(name);
        }
        
        mnemonics
    }

    #[wasm_bindgen(js_name = "getAvailableRegisters")]
    pub fn get_available_registers(&self) -> Vec<String> {
        use crate::types::Register;
        
        Register::all()
            .into_iter()
            .map(|r| r.to_string())
            .collect()
    }

    #[wasm_bindgen]
    pub fn assemble(&self, source: &str) -> Result<JsValue, JsValue> {
        match self.assembler.assemble(source) {
            Ok(obj) => to_value(&obj).map_err(|e| JsValue::from_str(&e.to_string())),
            Err(errors) => Err(JsValue::from_str(&errors.join("\n"))),
        }
    }

    #[wasm_bindgen(js_name = "assembleToBinary")]
    pub fn assemble_to_binary(&self, source: &str) -> Result<Vec<u8>, JsValue> {
        self.assembler.assemble_to_binary(source)
            .map_err(|errors| JsValue::from_str(&errors.join("\n")))
    }

    #[wasm_bindgen(js_name = "assembleToMacro")]
    pub fn assemble_to_macro(&self, source: &str) -> Result<String, JsValue> {
        match self.assembler.assemble(source) {
            Ok(obj) => {
                let formatter = MacroFormatter::new();
                Ok(formatter.format_full_program(
                    &obj.instructions,
                    Some(&obj.data),
                    None,
                    None,
                ))
            }
            Err(errors) => Err(JsValue::from_str(&errors.join("\n"))),
        }
    }
}

#[cfg(feature = "wasm")]
#[wasm_bindgen]
pub struct WasmLinker {
    linker: Linker,
}

#[cfg(feature = "wasm")]
#[wasm_bindgen]
impl WasmLinker {
    #[wasm_bindgen(constructor)]
    pub fn new(bank_size: u16) -> Self {
        Self {
            linker: Linker::new(bank_size),
        }
    }

    #[wasm_bindgen]
    pub fn link(&self, object_files: JsValue) -> Result<JsValue, JsValue> {
        let files: Vec<crate::ObjectFile> = from_value(object_files)
            .map_err(|e| JsValue::from_str(&e.to_string()))?;
        
        match self.linker.link(files) {
            Ok(program) => {
                // Convert LinkedProgram to a JS-friendly format
                let js_obj = js_sys::Object::new();
                
                js_sys::Reflect::set(
                    &js_obj,
                    &JsValue::from_str("instructions"),
                    &to_value(&program.instructions).unwrap(),
                ).unwrap();
                
                js_sys::Reflect::set(
                    &js_obj,
                    &JsValue::from_str("data"),
                    &to_value(&program.data).unwrap(),
                ).unwrap();
                
                js_sys::Reflect::set(
                    &js_obj,
                    &JsValue::from_str("entryPoint"),
                    &JsValue::from_f64(program.entry_point as f64),
                ).unwrap();
                
                Ok(js_obj.into())
            }
            Err(errors) => Err(JsValue::from_str(&errors.join("\n"))),
        }
    }

    #[wasm_bindgen(js_name = "linkToBinary")]
    pub fn link_to_binary(&self, object_files: JsValue) -> Result<Vec<u8>, JsValue> {
        let files: Vec<crate::ObjectFile> = from_value(object_files)
            .map_err(|e| JsValue::from_str(&e.to_string()))?;
        
        match self.linker.link(files) {
            Ok(program) => Ok(program.to_binary()),
            Err(errors) => Err(JsValue::from_str(&errors.join("\n"))),
        }
    }

    #[wasm_bindgen(js_name = "linkToMacro")]
    pub fn link_to_macro(&self, object_files: JsValue) -> Result<String, JsValue> {
        let files: Vec<crate::ObjectFile> = from_value(object_files)
            .map_err(|e| JsValue::from_str(&e.to_string()))?;
        
        match self.linker.link(files) {
            Ok(program) => {
                let formatter = MacroFormatter::new();
                Ok(formatter.format_linked_program(&program))
            }
            Err(errors) => Err(JsValue::from_str(&errors.join("\n"))),
        }
    }
}

#[cfg(feature = "wasm")]
#[wasm_bindgen]
pub struct WasmFormatter {
    formatter: MacroFormatter,
}

#[cfg(feature = "wasm")]
#[wasm_bindgen]
impl WasmFormatter {
    #[wasm_bindgen(constructor)]
    pub fn new() -> Self {
        Self {
            formatter: MacroFormatter::new(),
        }
    }

    #[wasm_bindgen(js_name = "formatObjectFile")]
    pub fn format_object_file(&self, obj: JsValue) -> Result<String, JsValue> {
        let object_file: crate::ObjectFile = from_value(obj)
            .map_err(|e| JsValue::from_str(&e.to_string()))?;
        
        Ok(self.formatter.format_full_program(
            &object_file.instructions,
            Some(&object_file.data),
            None,
            None,
        ))
    }
}