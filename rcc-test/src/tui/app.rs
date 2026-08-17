use std::collections::{HashMap, BTreeMap, HashSet};
use std::path::PathBuf;
use std::sync::mpsc;
use crate::config::{TestConfig, TestCase, KnownFailure, TestType, discover_orphan_tests};
use crate::compiler::ToolPaths;

fn fuzzy_match(text: &str, pattern: &str) -> bool {
    let mut pattern_chars = pattern.chars();
    let mut current_char = pattern_chars.next();
    
    for text_char in text.chars() {
        if let Some(pc) = current_char {
            if text_char == pc {
                current_char = pattern_chars.next();
            }
        } else {
            return true; // All pattern chars found
        }
    }
    
    current_char.is_none() // True if all pattern chars were found
}

#[derive(Debug, Clone)]
pub enum SelectedItemType {
    None,
    Category(String),
    Test(TestCase),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FocusedPane {
    TestList,
    RightPanel,  // Contains all tabs: Source, ASM, IR, Output, Details
    Filter,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AppMode {
    Normal,
    FindTest,  // Fuzzy finder mode
    Running,
    SelectCategory,
    AddingMetadata,  // Adding metadata to an orphan test
    ConfirmDelete,   // Confirming deletion of orphan test
    EditingExpected, // Editing expected output for a test
    RenamingTest,    // Renaming a test file
    MovingTest,      // Moving test to different category
    CreatingTest,    // Creating a new test from template
}

#[derive(Debug, Clone)]
pub struct CategoryView {
    pub name: String,
    pub tests: Vec<TestCase>,
    pub expanded: bool,
    pub test_count: usize,
}

impl CategoryView {
    pub fn from_tests(tests: &[TestCase], failures: &[KnownFailure], orphans: &[TestCase]) -> BTreeMap<String, CategoryView> {
        let mut categories: BTreeMap<String, Vec<TestCase>> = BTreeMap::new();
        
        // Group tests by category
        for test in tests {
            let category = Self::get_category_from_path(&test.file);
            categories.entry(category).or_default().push(test.clone());
        }
        
        // Add orphan tests - group them by their path structure
        for orphan in orphans {
            let category = Self::get_category_from_path(&orphan.file);
            // Mark orphan tests with a special prefix in their category
            let orphan_category = if category == "Uncategorized" {
                "Orphan Tests".to_string()
            } else {
                format!("Orphan › {category}")
            };
            categories.entry(orphan_category).or_default().push(orphan.clone());
        }
        
        // Add known failures as a category
        if !failures.is_empty() {
            let mut failure_tests = Vec::new();
            for failure in failures {
                let test_type = if failure.file.extension() == Some(std::ffi::OsStr::new("bfm")) {
                    TestType::Bfm
                } else {
                    TestType::C
                };
                failure_tests.push(TestCase {
                    file: failure.file.clone(),
                    expected: None,
                    use_runtime: test_type == TestType::C,
                    description: failure.description.clone(),
                    skipped: false,
                    test_type,
                    timeout_secs: None,
                });
            }
            categories.insert("Known Failures".to_string(), failure_tests);
        }
        
        // Convert to CategoryView and sort tests within each category
        let mut result = BTreeMap::new();
        for (name, mut tests) in categories {
            // Sort tests alphabetically within the category
            tests.sort_by(|a, b| a.file.cmp(&b.file));
            
            result.insert(name.clone(), CategoryView {
                name: name.clone(),
                test_count: tests.len(),
                tests,
                expanded: false, // Start collapsed
            });
        }
        
        result
    }
    
    fn get_category_from_path(path: &PathBuf) -> String {
        let path_str = path.to_string_lossy();
        let parts: Vec<&str> = path_str.split('/').collect();
        
        // Find where test directories start
        let mut start_idx = None;
        for (i, part) in parts.iter().enumerate() {
            if *part == "tests" || *part == "tests-runtime" || 
               *part == "known-failures" || *part == "examples" {
                start_idx = Some(i);
                break;
            }
        }
        
        if let Some(idx) = start_idx {
            let root = parts[idx];
            
            match root {
                "tests-runtime" => "Runtime".to_string(),
                "examples" => "Examples".to_string(),
                "known-failures" => {
                    // Check for subdirectory
                    if parts.len() > idx + 2 {
                        format!("Known Failures › {}", Self::capitalize_words(parts[idx + 1]))
                    } else {
                        "Known Failures".to_string()
                    }
                },
                "tests" => {
                    // Build category from path components
                    let mut category_parts = Vec::new();
                    for i in (idx + 1)..(parts.len() - 1) {
                        category_parts.push(Self::capitalize_words(parts[i]));
                    }
                    
                    if category_parts.is_empty() {
                        "Uncategorized".to_string()
                    } else {
                        category_parts.join(" › ")
                    }
                },
                _ => "Uncategorized".to_string()
            }
        } else {
            "Uncategorized".to_string()
        }
    }
    
    fn capitalize_words(s: &str) -> String {
        s.split('-')
            .map(|word| {
                let mut chars = word.chars();
                match chars.next() {
                    None => String::new(),
                    Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
                }
            })
            .collect::<Vec<_>>()
            .join(" ")
    }
}

#[derive(Debug, Clone)]
pub struct TestResult {
    pub passed: bool,
    pub output: String,
    pub expected: Option<String>,
    pub duration_ms: u128,
}

#[derive(Debug, Clone)]
pub enum TestMessage {
    Started(String),
    Completed(String, TestResult),
    BatchCompleted(Vec<(String, TestResult)>),
    Progress(String),
    Finished,
}

pub struct TuiApp {
    // UI State
    pub focused_pane: FocusedPane,
    pub mode: AppMode,
    pub selected_tab: usize,  // 0=Source, 1=ASM, 2=IR, 3=Output, 4=Details, 5=AST, 6=Symbols, 7=TypedAST

    // Test data
    pub test_config: TestConfig,
    pub tools: ToolPaths,
    pub filtered_tests: Vec<TestCase>,
    pub filtered_failures: Vec<KnownFailure>,
    pub orphan_tests: Vec<TestCase>,  // Tests without metadata

    // Selection and scrolling
    pub selected_test: usize,
    pub selected_item: usize,  // Index in the flattened list view
    pub test_scroll: usize,
    pub output_scroll: usize,
    pub source_scroll: usize,
    pub asm_scroll: usize,
    pub ir_scroll: usize,
    pub details_scroll: usize,
    pub category_scroll: usize,
    
    // Trace viewer state
    pub ast_scroll: usize,
    pub ast_selected_path: Vec<usize>,
    pub ast_expanded_nodes: HashSet<Vec<usize>>,
    pub ast_tree: Option<crate::tui::ui::trace_viewer::TreeNode>,
    
    pub symbols_scroll: usize,
    
    pub typed_ast_scroll: usize,
    pub typed_ast_selected_path: Vec<usize>,
    pub typed_ast_expanded_nodes: HashSet<Vec<usize>>,
    pub typed_ast_tree: Option<crate::tui::ui::trace_viewer::TreeNode>,

    // Categories and filtering
    pub categories: BTreeMap<String, CategoryView>,
    pub selected_category: Option<String>,
    pub selected_category_index: usize,
    pub show_categories: bool,
    
    // Fuzzy finder
    pub search_query: String,
    pub search_results: Vec<TestCase>,
    pub search_selected_index: usize,

    // Test execution
    pub test_results: HashMap<String, TestResult>,
    pub running_test: Option<String>,
    pub output_buffer: String,
    
    // Batch execution progress
    pub test_receiver: Option<mpsc::Receiver<TestMessage>>,
    pub tests_total: usize,
    pub tests_completed: usize,

    // Metadata input for orphan tests
    pub metadata_input: MetadataInput,
    
    // Delete confirmation
    pub delete_target: Option<PathBuf>,
    
    // Expected output editing
    pub editing_test_file: Option<PathBuf>,
    pub editing_expected: String,
    
    // Rename/move operations
    pub rename_new_name: String,
    pub move_target_category: String,
    
    // Create new test
    pub new_test_name: String,
    pub new_test_description: String,
    pub new_test_focused_field: bool, // false = name, true = description

    // Settings
    pub bank_size: usize,
    pub timeout_secs: u64,
    pub show_help: bool,
    pub help_scroll: usize,
}

#[derive(Debug, Clone)]
pub struct MetadataInput {
    pub test_file: Option<PathBuf>,
    pub expected_output: String,
    pub use_runtime: bool,
    pub is_known_failure: bool,
    pub description: String,
    pub focused_field: MetadataField,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MetadataField {
    ExpectedOutput,
    Description,
    UseRuntime,
    IsKnownFailure,
}

impl Default for MetadataInput {
    fn default() -> Self {
        Self {
            test_file: None,
            expected_output: String::new(),
            use_runtime: true,
            is_known_failure: false,
            description: String::new(),
            focused_field: MetadataField::ExpectedOutput,
        }
    }
}

impl TuiApp {
    pub fn new(test_config: TestConfig, tools: ToolPaths, bank_size: usize, timeout_secs: u64) -> Self {
        // Discover orphan tests
        let orphan_tests = discover_orphan_tests().unwrap_or_else(|_| Vec::new());
        
        let categories = CategoryView::from_tests(&test_config.tests, &test_config.known_failures, &orphan_tests);
        let mut app = Self {
            focused_pane: FocusedPane::TestList,
            mode: AppMode::Normal,
            selected_tab: 0,

            filtered_tests: test_config.tests.clone(),
            filtered_failures: test_config.known_failures.clone(),
            orphan_tests,
            test_config,
            tools,

            selected_test: 0,
            selected_item: 0,
            test_scroll: 0,
            output_scroll: 0,
            source_scroll: 0,
            asm_scroll: 0,
            ir_scroll: 0,
            details_scroll: 0,
            category_scroll: 0,
            
            // Trace viewer state
            ast_scroll: 0,
            ast_selected_path: vec![],
            ast_expanded_nodes: {                
                let mut set = HashSet::new();
                set.insert(vec![]);  // Root is always expanded initially
                set
            },
            ast_tree: None,
            
            symbols_scroll: 0,
            
            typed_ast_scroll: 0,
            typed_ast_selected_path: vec![],
            typed_ast_expanded_nodes: {
                let mut set = HashSet::new();
                set.insert(vec![]);  // Root is always expanded initially
                set
            },
            typed_ast_tree: None,

            categories,
            selected_category: None,
            selected_category_index: 0,
            show_categories: false,
            
            search_query: String::new(),
            search_results: Vec::new(),
            search_selected_index: 0,

            test_results: HashMap::new(),
            running_test: None,
            output_buffer: String::new(),
            test_receiver: None,
            tests_total: 0,
            tests_completed: 0,

            metadata_input: MetadataInput::default(),
            delete_target: None,
            editing_test_file: None,
            editing_expected: String::new(),
            rename_new_name: String::new(),
            move_target_category: String::new(),
            new_test_name: String::new(),
            new_test_description: String::new(),
            new_test_focused_field: false,

            bank_size,
            timeout_secs,
            show_help: false,
            help_scroll: 0,
        };

        app.apply_filters();
        app
    }

    pub fn apply_filters(&mut self) {
        // Filter by category
        if let Some(ref category_name) = self.selected_category {
            if let Some(category) = self.categories.get(category_name) {
                self.filtered_tests = category.tests.clone();
                self.filtered_failures = Vec::new();
            } else if category_name == "Known Failures" {
                self.filtered_tests = Vec::new();
                self.filtered_failures = self.test_config.known_failures.clone();
            } else {
                self.filtered_tests = Vec::new();
                self.filtered_failures = Vec::new();
            }
        } else {
            // No category selected, show all
            self.filtered_tests = self.test_config.tests.clone();
            self.filtered_failures = self.test_config.known_failures.clone();
        }

        // Reset selection if out of bounds
        let total_items = self.filtered_tests.len() + self.filtered_failures.len();
        if self.selected_test >= total_items && total_items > 0 {
            self.selected_test = total_items - 1;
        }
    }
    
    pub fn update_search_results(&mut self) {
        if self.search_query.is_empty() {
            self.search_results.clear();
            return;
        }
        
        let query_lower = self.search_query.to_lowercase();
        self.search_results.clear();
        
        // Search through all tests
        for test in &self.test_config.tests {
            let test_name = test.file.file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("");
            let test_path = test.file.to_string_lossy();
            
            // Fuzzy matching: check if query chars appear in order
            if fuzzy_match(&test_name.to_lowercase(), &query_lower) ||
               fuzzy_match(&test_path.to_lowercase(), &query_lower) ||
               test.description.as_ref()
                   .map(|d| fuzzy_match(&d.to_lowercase(), &query_lower))
                   .unwrap_or(false) {
                self.search_results.push(test.clone());
            }
        }
        
        // Sort by relevance (prefer matches in test name over path)
        self.search_results.sort_by(|a, b| {
            let a_name = a.file.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            let b_name = b.file.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            
            let a_score = if a_name.to_lowercase().contains(&query_lower) { 0 } else { 1 };
            let b_score = if b_name.to_lowercase().contains(&query_lower) { 0 } else { 1 };
            
            a_score.cmp(&b_score).then_with(|| a_name.cmp(b_name))
        });
        
        // Reset selection
        self.search_selected_index = 0;
    }
    
    pub fn jump_to_selected_search_result(&mut self) {
        if let Some(selected_test) = self.search_results.get(self.search_selected_index).cloned() {
            // Find the test in the main list and set selection to it
            self.jump_to_test(&selected_test);
        }
    }
    
    pub fn jump_to_test(&mut self, target_test: &TestCase) {
        let mut current_idx = 0;
        
        // Search through all visible items
        for category in self.categories.values() {
            current_idx += 1; // Category header
            
            if category.expanded {
                for test in &category.tests {
                    if test.file == target_test.file {
                        self.selected_item = current_idx;
                        self.ensure_selection_visible();
                        return;
                    }
                    current_idx += 1;
                }
            }
        }
    }
    
    pub fn jump_to_test_by_name(&mut self, test_name: &str) {
        // Strip .c extension if provided
        let test_name = test_name.strip_suffix(".c").unwrap_or(test_name);
        
        let mut current_idx = 0;
        
        // Search through all visible items
        for category in self.categories.values() {
            current_idx += 1; // Category header
            
            if category.expanded {
                for test in &category.tests {
                    // Check if the file name matches
                    if let Some(file_stem) = test.file.file_stem() {
                        if file_stem.to_str() == Some(test_name) {
                            self.selected_item = current_idx;
                            self.ensure_selection_visible();
                            return;
                        }
                    }
                    current_idx += 1;
                }
            }
        }
        
        // If not found, try to expand categories and search again
        for category in self.categories.values_mut() {
            category.expanded = true;
        }
        
        current_idx = 0;
        for category in self.categories.values() {
            current_idx += 1; // Category header
            
            for test in &category.tests {
                if let Some(file_stem) = test.file.file_stem() {
                    if file_stem.to_str() == Some(test_name) {
                        self.selected_item = current_idx;
                        self.ensure_selection_visible();
                        return;
                    }
                }
                current_idx += 1;
            }
        }
    }
    
    pub fn jump_to_first_orphan(&mut self) {
        // First, find if there's an orphan test and its location
        let mut found_orphan: Option<(String, usize)> = None;
        let mut current_idx = 0;
        
        for (name, category) in self.categories.iter() {
            current_idx += 1; // Category header
            
            // Check if this is an orphan category
            if name.starts_with("Orphan") && !category.tests.is_empty() {
                found_orphan = Some((name.clone(), current_idx));
                break;
            } else if category.expanded {
                // Check tests in non-orphan categories for orphan tests
                for (test_idx, test) in category.tests.iter().enumerate() {
                    if self.orphan_tests.iter().any(|orphan| orphan.file == test.file) {
                        found_orphan = Some((name.clone(), current_idx + test_idx));
                        break;
                    }
                }
                if found_orphan.is_some() {
                    break;
                }
                current_idx += category.tests.len();
            } else {
                // Category is collapsed, check if it contains orphans
                for test in &category.tests {
                    if self.orphan_tests.iter().any(|orphan| orphan.file == test.file) {
                        found_orphan = Some((name.clone(), current_idx));
                        break;
                    }
                }
                if found_orphan.is_some() {
                    break;
                }
            }
        }
        
        // Now apply the changes if we found an orphan
        if let Some((category_name, position)) = found_orphan {
            // Expand the category if needed
            if let Some(category) = self.categories.get_mut(&category_name) {
                if !category.expanded {
                    category.expanded = true;
                }
            }
            
            // Jump to the position
            self.selected_item = position;
            self.ensure_selection_visible();
            
            // If we're on a category header, move down to the first test
            if self.categories.contains_key(&category_name) && category_name.starts_with("Orphan") {
                self.move_selection_down();
            }
        } else {
            // If no orphan found, show a message in output
            self.append_output("No orphan tests found.\n");
        }
    }

    pub fn get_selected_test_name(&self) -> Option<String> {
        match self.get_selected_item_type() {
            SelectedItemType::Test(test) => {
                test.file.file_stem()
                    .and_then(|s| s.to_str())
                    .map(|s| s.to_string())
            }
            _ => None
        }
    }

    pub fn get_selected_test_path(&self) -> Option<PathBuf> {
        match self.get_selected_item_type() {
            SelectedItemType::Test(test) => Some(test.file),
            _ => None
        }
    }

    pub fn get_selected_test_details(&self) -> Option<TestCase> {
        match self.get_selected_item_type() {
            SelectedItemType::Test(test) => Some(test),
            _ => None
        }
    }

    pub fn move_selection_up(&mut self) {
        let total_items = self.get_total_visible_items();
        if total_items == 0 {
            return;
        }
        
        if self.selected_item > 0 {
            self.selected_item -= 1;
        } else {
            // Wrap to last item
            self.selected_item = total_items - 1;
        }
        self.ensure_selection_visible();
    }

    pub fn move_selection_down(&mut self) {
        let total_items = self.get_total_visible_items();
        if total_items == 0 {
            return;
        }
        
        if self.selected_item < total_items - 1 {
            self.selected_item += 1;
        } else {
            // Wrap to first item
            self.selected_item = 0;
        }
        self.ensure_selection_visible();
    }
    
    pub fn get_total_visible_items(&self) -> usize {
        let mut count = 0;
        
        if let Some(ref selected_cat) = self.selected_category {
            // Single category view
            count += 1; // Category header
            if let Some(category) = self.categories.get(selected_cat) {
                if category.expanded {
                    count += category.tests.len();
                }
            }
        } else {
            // All categories view
            for category in self.categories.values() {
                count += 1; // Category header
                if category.expanded {
                    count += category.tests.len();
                }
            }
        }
        
        count
    }
    
    pub fn toggle_current_category(&mut self) {
        let mut current_idx = 0;
        
        if let Some(ref selected_cat) = self.selected_category {
            // Single category view - toggle it if header is selected
            if self.selected_item == 0 {
                if let Some(category) = self.categories.get_mut(selected_cat) {
                    category.expanded = !category.expanded;
                }
            }
        } else {
            // All categories view - find which category header is selected
            for (_name, category) in self.categories.iter_mut() {
                if current_idx == self.selected_item {
                    // This is the selected category header
                    category.expanded = !category.expanded;
                    return;
                }
                current_idx += 1;
                
                if category.expanded {
                    current_idx += category.tests.len();
                    if current_idx > self.selected_item {
                        // Selected item is a test, not a header
                        return;
                    }
                }
            }
        }
    }
    
    pub fn get_selected_item_type(&self) -> SelectedItemType {
        let mut current_idx = 0;
        
        if let Some(ref selected_cat) = self.selected_category {
            // Single category view
            if self.selected_item == 0 {
                return SelectedItemType::Category(selected_cat.clone());
            }
            if let Some(category) = self.categories.get(selected_cat) {
                if category.expanded && self.selected_item > 0 && self.selected_item <= category.tests.len() {
                    if let Some(test) = category.tests.get(self.selected_item - 1) {
                        return SelectedItemType::Test(test.clone());
                    }
                }
            }
        } else {
            // All categories view
            for (name, category) in &self.categories {
                if current_idx == self.selected_item {
                    return SelectedItemType::Category(name.clone());
                }
                current_idx += 1;
                
                if category.expanded {
                    for test in &category.tests {
                        if current_idx == self.selected_item {
                            return SelectedItemType::Test(test.clone());
                        }
                        current_idx += 1;
                    }
                }
            }
        }
        
        SelectedItemType::None
    }
    
    pub fn get_current_category_name(&self) -> Option<String> {
        let mut current_idx = 0;
        
        if let Some(ref selected_cat) = self.selected_category {
            // Single category view - always return this category
            return Some(selected_cat.clone());
        }
        
        // All categories view - find which category the cursor is in
        for (name, category) in &self.categories {
            if current_idx == self.selected_item {
                // Cursor is on category header
                return Some(name.clone());
            }
            current_idx += 1;
            
            if category.expanded {
                let category_end = current_idx + category.tests.len();
                if self.selected_item < category_end {
                    // Cursor is on a test within this category
                    return Some(name.clone());
                }
                current_idx = category_end;
            }
        }
        
        None
    }
    
    pub fn get_category_tests(&self, category_name: &str) -> Vec<TestCase> {
        self.categories
            .get(category_name)
            .map(|cat| cat.tests.clone())
            .unwrap_or_default()
    }

    pub fn ensure_selection_visible(&mut self) {
        // Adjust scroll to keep selection visible
        const VISIBLE_ITEMS: usize = 20; // Approximate visible items in list
        
        if self.selected_item < self.test_scroll {
            self.test_scroll = self.selected_item;
        } else if self.selected_item >= self.test_scroll + VISIBLE_ITEMS {
            self.test_scroll = self.selected_item.saturating_sub(VISIBLE_ITEMS - 1);
        }
    }

    pub fn toggle_category_selection(&mut self) {
        self.show_categories = !self.show_categories;
        if self.show_categories {
            self.mode = AppMode::SelectCategory;
        } else {
            self.mode = AppMode::Normal;
        }
    }

    pub fn select_category(&mut self, category_name: String) {
        self.selected_category = Some(category_name);
        self.apply_filters();
        self.show_categories = false;
        self.mode = AppMode::Normal;
    }
    
    pub fn clear_category(&mut self) {
        self.selected_category = None;
        self.apply_filters();
    }

    pub fn start_find_test(&mut self) {
        self.mode = AppMode::FindTest;
        self.search_query.clear();
        self.search_results.clear();
        self.search_selected_index = 0;
    }

    pub fn close_find_test(&mut self) {
        self.mode = AppMode::Normal;
        // Keep search query for next time
    }

    pub fn append_output(&mut self, text: &str) {
        self.output_buffer.push_str(text);
        // Don't auto-scroll, let user control scrolling
    }

    pub fn clear_output(&mut self) {
        self.output_buffer.clear();
        self.output_scroll = 0;
    }

    pub fn record_test_result(&mut self, test_name: String, result: TestResult) {
        self.test_results.insert(test_name, result);
        self.running_test = None;
    }

    pub fn move_category_selection_up(&mut self) {
        let total_categories = self.categories.len() + 1; // +1 for "All Tests" option
        if self.selected_category_index > 0 {
            self.selected_category_index -= 1;
        } else {
            // Wrap to last category
            self.selected_category_index = total_categories - 1;
        }
    }

    pub fn move_category_selection_down(&mut self) {
        let total_categories = self.categories.len() + 1; // +1 for "All Tests" option
        // Wrap around using modulo
        self.selected_category_index = (self.selected_category_index + 1) % total_categories;
    }

    pub fn select_current_category(&mut self) {
        if self.selected_category_index == 0 {
            // "All Tests" option
            self.clear_category();
        } else {
            // Get the category at the current index
            let category_names: Vec<String> = self.categories.keys().cloned().collect();
            if let Some(category_name) = category_names.get(self.selected_category_index - 1) {
                self.select_category(category_name.clone());
            }
        }
    }

    pub fn is_current_test_orphan(&self) -> bool {
        if let Some(test) = self.get_selected_test_details() {
            // Check if this test is in the orphan list
            self.orphan_tests.iter().any(|orphan| orphan.file == test.file)
        } else {
            false
        }
    }

    pub fn start_adding_metadata(&mut self) {
        if let Some(test) = self.get_selected_test_details() {
            if self.is_current_test_orphan() {
                self.metadata_input = MetadataInput {
                    test_file: Some(test.file.clone()),
                    expected_output: String::new(),
                    use_runtime: true,
                    is_known_failure: false,
                    description: String::new(),
                    focused_field: MetadataField::ExpectedOutput,
                };
                self.mode = AppMode::AddingMetadata;
            }
        }
    }
    
    pub fn quick_add_orphan_metadata(&mut self) -> anyhow::Result<()> {
        // Check if current test is an orphan
        if !self.is_current_test_orphan() {
            self.append_output("Current test is not an orphan test.\n");
            return Ok(());
        }
        
        // Get the test details
        let test = match self.get_selected_test_details() {
            Some(t) => t,
            None => {
                self.append_output("No test selected.\n");
                return Ok(());
            }
        };
        
        // Get the test name for looking up the result
        let test_name = test.file.file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("unknown");
        
        // Get the last test result output
        let output = self.test_results.get(test_name)
            .map(|r| r.output.clone())
            .unwrap_or_default();
        
        if output.is_empty() {
            self.append_output("No test output available. Run the test first.\n");
            return Ok(());
        }
        
        // Set up metadata with current output as expected
        self.metadata_input = MetadataInput {
            test_file: Some(test.file.clone()),
            expected_output: output.clone(),
            use_runtime: true,  // Default to true
            is_known_failure: false,
            description: String::new(),
            focused_field: MetadataField::ExpectedOutput,
        };
        
        // Save the metadata immediately
        self.save_metadata()?;
        
        // Clear the metadata input state
        self.metadata_input = MetadataInput::default();
        
        self.append_output(&format!("Added metadata for {test_name} with current output as expected.\n"));
        Ok(())
    }

    pub fn start_delete_test(&mut self) {
        if let Some(test) = self.get_selected_test_details() {
            // Allow deletion of any test (orphan or regular)
            self.delete_target = Some(test.file.clone());
            self.mode = AppMode::ConfirmDelete;
        }
    }
    
    pub fn confirm_delete_test(&mut self) -> anyhow::Result<()> {
        if let Some(test_file) = &self.delete_target {
            // Construct full path
            let full_path = if test_file.is_relative() && !test_file.starts_with("c-test") {
                PathBuf::from("c-test").join(test_file)
            } else {
                test_file.clone()
            };
            
            // Delete the C file
            std::fs::remove_file(&full_path)?;
            
            // Also delete the .meta.json file if it exists
            let meta_path = full_path.with_extension("meta.json");
            if meta_path.exists() {
                let _ = std::fs::remove_file(&meta_path);
            }
            
            // Remove from appropriate list
            let is_orphan = self.orphan_tests.iter().any(|t| t.file == *test_file);
            
            if is_orphan {
                // Remove from orphan list
                if let Some(idx) = self.orphan_tests.iter().position(|t| t.file == *test_file) {
                    self.orphan_tests.remove(idx);
                }
            } else {
                // Remove from regular tests or known failures
                if let Some(idx) = self.test_config.tests.iter().position(|t| t.file == *test_file) {
                    self.test_config.tests.remove(idx);
                } else if let Some(idx) = self.test_config.known_failures.iter().position(|t| t.file == *test_file) {
                    self.test_config.known_failures.remove(idx);
                }
            }
            
            // Rebuild categories
            self.categories = CategoryView::from_tests(
                &self.test_config.tests,
                &self.test_config.known_failures,
                &self.orphan_tests
            );
            
            // Clear delete target and return to normal mode
            self.delete_target = None;
            self.mode = AppMode::Normal;
        }
        
        Ok(())
    }
    
    pub fn cancel_delete(&mut self) {
        self.delete_target = None;
        self.mode = AppMode::Normal;
    }
    
    pub fn start_edit_expected(&mut self) {
        if let Some(test) = self.get_selected_test_details() {
            self.editing_test_file = Some(test.file.clone());
            self.editing_expected = test.expected.clone().unwrap_or_default();
            self.mode = AppMode::EditingExpected;
        }
    }
    
    pub fn save_expected_output(&mut self) -> anyhow::Result<()> {
        if let Some(test_file) = &self.editing_test_file {
            // Load existing metadata or create new
            let full_path = if test_file.is_relative() && !test_file.starts_with("c-test") {
                PathBuf::from("c-test").join(test_file)
            } else {
                test_file.clone()
            };
            
            let meta_path = full_path.with_extension("meta.json");
            
            let mut metadata = if meta_path.exists() {
                let content = std::fs::read_to_string(&meta_path)?;
                serde_json::from_str::<crate::config::TestMetadata>(&content)?
            } else {
                // Create new metadata for orphan test
                let test_type = if test_file.extension() == Some(std::ffi::OsStr::new("bfm")) {
                    TestType::Bfm
                } else {
                    TestType::C
                };
                crate::config::TestMetadata {
                    expected: None,
                    use_runtime: test_type == TestType::C,
                    description: None,
                    known_failure: false,
                    category: None,
                    skipped: false,
                    test_type,
                    timeout_secs: None,
                }
            };
            
            // Update expected output
            metadata.expected = if self.editing_expected.is_empty() {
                None
            } else {
                Some(self.editing_expected.clone())
            };
            
            // Save metadata
            let content = serde_json::to_string_pretty(&metadata)?;
            std::fs::write(&meta_path, content)?;
            
            // Update the test in our config
            if let Some(test) = self.test_config.tests.iter_mut()
                .find(|t| t.file == *test_file) {
                test.expected = metadata.expected.clone();
            }
            
            // If this was an orphan, it's no longer one
            if let Some(idx) = self.orphan_tests.iter().position(|t| t.file == *test_file) {
                let orphan = self.orphan_tests.remove(idx);
                let test_type = if orphan.file.extension() == Some(std::ffi::OsStr::new("bfm")) {
                    TestType::Bfm
                } else {
                    TestType::C
                };
                self.test_config.tests.push(TestCase {
                    file: orphan.file,
                    expected: metadata.expected,
                    use_runtime: test_type == TestType::C && metadata.use_runtime,
                    description: metadata.description,
                    skipped: metadata.skipped,
                    test_type,
                    timeout_secs: metadata.timeout_secs,
                });
                
                // Rebuild categories
                self.categories = CategoryView::from_tests(
                    &self.test_config.tests,
                    &self.test_config.known_failures,
                    &self.orphan_tests
                );
            }
            
            // Clear editing state
            self.editing_test_file = None;
            self.editing_expected.clear();
            self.mode = AppMode::Normal;
            
            self.append_output("Expected output updated successfully.\n");
        }
        
        Ok(())
    }
    
    pub fn cancel_edit_expected(&mut self) {
        self.editing_test_file = None;
        self.editing_expected.clear();
        self.mode = AppMode::Normal;
    }
    
    pub fn apply_golden_output(&mut self) -> anyhow::Result<()> {
        // Get the current test's actual output from the last run
        if let Some(test_name) = self.get_selected_test_name() {
            // Clone the values we need before mutating self
            let result_info = self.test_results.get(&test_name).map(|r| (r.passed, r.output.clone()));
            
            if let Some((passed, output)) = result_info {
                if !passed {
                    // Start editing with the actual output
                    self.start_edit_expected();
                    self.editing_expected = output;
                    
                    // Auto-save immediately for golden update
                    self.save_expected_output()?;
                    self.append_output(&format!("Updated expected output for {test_name} to match actual output.\n"));
                } else {
                    self.append_output("Test is already passing, no need to update expected output.\n");
                }
            } else {
                self.append_output("No test result available. Run the test first.\n");
            }
        }
        Ok(())
    }
    
    pub fn toggle_skip_status(&mut self) -> anyhow::Result<()> {
        // Get the current selected test
        if let Some(test) = self.get_selected_test_details() {
            let test_path = test.file.clone();
            
            // Find and update the test in the test_config
            for test_case in &mut self.test_config.tests {
                if test_case.file == test_path {
                    // Toggle the skipped status
                    test_case.skipped = !test_case.skipped;
                    
                    // Save to .meta.json file
                    let full_path = if test_path.is_relative() && !test_path.starts_with("c-test") {
                        PathBuf::from("c-test").join(&test_path)
                    } else {
                        test_path.clone()
                    };
                    
                    let meta_path = full_path.with_extension("meta.json");
                    
                    // Read existing metadata or create new
                    let mut metadata: HashMap<String, serde_json::Value> = if meta_path.exists() {
                        let content = std::fs::read_to_string(&meta_path)?;
                        serde_json::from_str(&content)?
                    } else {
                        HashMap::new()
                    };
                    
                    // Update the skipped field
                    if test_case.skipped {
                        metadata.insert("skipped".to_string(), serde_json::Value::Bool(true));
                    } else {
                        // Remove the skipped field if false (to keep files clean)
                        metadata.remove("skipped");
                    }
                    
                    // Preserve other fields
                    if let Some(expected) = &test_case.expected {
                        metadata.insert("expected".to_string(), serde_json::Value::String(expected.clone()));
                    }
                    metadata.insert("use_runtime".to_string(), serde_json::Value::Bool(test_case.use_runtime));
                    if let Some(desc) = &test_case.description {
                        metadata.insert("description".to_string(), serde_json::Value::String(desc.clone()));
                    }
                    
                    // Write the updated metadata
                    let content = serde_json::to_string_pretty(&metadata)?;
                    std::fs::write(&meta_path, content)?;
                    
                    // Update in filtered lists too
                    for filtered in &mut self.filtered_tests {
                        if filtered.file == test_path {
                            filtered.skipped = test_case.skipped;
                            break;
                        }
                    }
                    
                    // Update in categories
                    for category in self.categories.values_mut() {
                        for cat_test in &mut category.tests {
                            if cat_test.file == test_path {
                                cat_test.skipped = test_case.skipped;
                                break;
                            }
                        }
                    }
                    
                    let status = if test_case.skipped { "skipped" } else { "enabled" };
                    self.append_output(&format!("Test '{}' is now {}.\n", 
                        test_path.file_stem().and_then(|s| s.to_str()).unwrap_or("unknown"),
                        status
                    ));
                    
                    return Ok(());
                }
            }
            
            self.append_output("Could not find test to update.\n");
        } else {
            self.append_output("No test selected.\n");
        }
        Ok(())
    }
    
    pub fn get_selected_test_path_for_edit(&self) -> Option<PathBuf> {
        if let Some(test) = self.get_selected_test_details() {
            // Construct full path
            let full_path = if test.file.is_relative() && !test.file.starts_with("c-test") {
                PathBuf::from("c-test").join(&test.file)
            } else {
                test.file.clone()
            };
            Some(full_path)
        } else {
            None
        }
    }
    
    pub fn refresh_test_content(&mut self) {
        // Mark that we should refresh the source view
        // Reset scroll position to see the beginning of the file
        self.source_scroll = 0;
        self.selected_tab = 0; // Switch to source tab to show the edited content
    }
    
    pub fn reload_all_tests(&mut self) {
        // Re-discover all tests from filesystem
        match crate::config::discover_tests() {
            Ok(new_config) => {
                self.test_config = new_config;
                
                // Re-discover orphan tests
                self.orphan_tests = discover_orphan_tests().unwrap_or_else(|_| Vec::new());
                
                // Update filtered lists
                self.filtered_tests = self.test_config.tests.clone();
                self.filtered_failures = self.test_config.known_failures.clone();
                
                // Rebuild categories
                self.categories = CategoryView::from_tests(
                    &self.test_config.tests,
                    &self.test_config.known_failures,
                    &self.orphan_tests
                );
                
                // Clear test results as they may be stale
                self.test_results.clear();
                
                // Reset selection if it's out of bounds
                let total_items = self.get_total_visible_items();
                if self.selected_item >= total_items && total_items > 0 {
                    self.selected_item = total_items - 1;
                }
                
                // Apply any existing filters
                self.apply_filters();
                
                self.append_output("Tests reloaded successfully.\n");
            }
            Err(e) => {
                self.append_output(&format!("Failed to reload tests: {e}\n"));
            }
        }
    }
    

    pub fn start_rename_test(&mut self) {
        if let Some(test) = self.get_selected_test_details() {
            // Get the test file name without extension
            let current_name = test.file.file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("")
                .to_string();
            self.rename_new_name = current_name;
            self.editing_test_file = Some(test.file.clone());
            self.mode = AppMode::RenamingTest;
        }
    }
    
    pub fn save_rename_test(&mut self) -> anyhow::Result<()> {
        if let Some(old_file) = &self.editing_test_file {
            if self.rename_new_name.is_empty() {
                return Err(anyhow::anyhow!("New name cannot be empty"));
            }
            
            // Construct full old path
            let old_full_path = if old_file.is_relative() && !old_file.starts_with("c-test") {
                PathBuf::from("c-test").join(old_file)
            } else {
                old_file.clone()
            };
            
            // Create new path with new name
            let new_name_with_ext = format!("{}.c", self.rename_new_name);
            let new_full_path = old_full_path.parent()
                .ok_or_else(|| anyhow::anyhow!("Cannot get parent directory"))?
                .join(new_name_with_ext);
            
            // Check if new file already exists
            if new_full_path.exists() && new_full_path != old_full_path {
                return Err(anyhow::anyhow!("A test with this name already exists"));
            }
            
            // Rename the C file
            if old_full_path != new_full_path {
                std::fs::rename(&old_full_path, &new_full_path)?;
                
                // Also rename the .meta.json file if it exists
                let old_meta_path = old_full_path.with_extension("meta.json");
                if old_meta_path.exists() {
                    let new_meta_path = new_full_path.with_extension("meta.json");
                    std::fs::rename(&old_meta_path, &new_meta_path)?;
                }
                
                // Update the test in our config
                let new_relative_path = if new_full_path.starts_with("c-test/") {
                    new_full_path.strip_prefix("c-test/")?.to_path_buf()
                } else if let Ok(rel) = new_full_path.strip_prefix(std::env::current_dir()?.join("c-test")) {
                    rel.to_path_buf()
                } else {
                    // Try to make it relative to c-test
                    let path_str = new_full_path.to_string_lossy();
                    if let Some(idx) = path_str.find("c-test/") {
                        PathBuf::from(&path_str[idx + 7..])
                    } else {
                        new_full_path.clone()
                    }
                };
                
                // Update in test config
                if let Some(test) = self.test_config.tests.iter_mut()
                    .find(|t| t.file == *old_file) {
                    test.file = new_relative_path.clone();
                }
                
                // Update in orphan tests if applicable
                if let Some(test) = self.orphan_tests.iter_mut()
                    .find(|t| t.file == *old_file) {
                    test.file = new_relative_path;
                }
                
                // Rebuild categories
                self.categories = CategoryView::from_tests(
                    &self.test_config.tests,
                    &self.test_config.known_failures,
                    &self.orphan_tests
                );
                
                self.append_output(&format!("Test renamed successfully to {}\n", self.rename_new_name));
            }
            
            // Clear rename state
            self.editing_test_file = None;
            self.rename_new_name.clear();
            self.mode = AppMode::Normal;
        }
        
        Ok(())
    }
    
    pub fn cancel_rename(&mut self) {
        self.editing_test_file = None;
        self.rename_new_name.clear();
        self.mode = AppMode::Normal;
    }
    
    pub fn start_move_test(&mut self) {
        if let Some(test) = self.get_selected_test_details() {
            self.editing_test_file = Some(test.file.clone());
            // Show category selector for choosing destination
            self.show_categories = true;
            self.mode = AppMode::MovingTest;
            // Reset category selection to first item
            self.selected_category_index = 0;
        }
    }
    
    pub fn save_move_test(&mut self, target_category: String) -> anyhow::Result<()> {
        if let Some(old_file) = &self.editing_test_file {
            // Parse target category to determine directory
            let target_dir = self.get_directory_for_category(&target_category)?;
            
            // Construct full old path
            let old_full_path = if old_file.is_relative() && !old_file.starts_with("c-test") {
                PathBuf::from("c-test").join(old_file)
            } else {
                old_file.clone()
            };
            
            // Create new path in target directory
            let file_name = old_full_path.file_name()
                .ok_or_else(|| anyhow::anyhow!("Cannot get file name"))?;
            let new_full_path = PathBuf::from("c-test").join(&target_dir).join(file_name);
            
            // Create target directory if it doesn't exist
            if let Some(parent) = new_full_path.parent() {
                std::fs::create_dir_all(parent)?;
            }
            
            // Check if new file already exists
            if new_full_path.exists() && new_full_path != old_full_path {
                return Err(anyhow::anyhow!("A test with this name already exists in the target category"));
            }
            
            // Move the C file
            if old_full_path != new_full_path {
                std::fs::rename(&old_full_path, &new_full_path)?;
                
                // Also move the .meta.json file if it exists
                let old_meta_path = old_full_path.with_extension("meta.json");
                if old_meta_path.exists() {
                    let new_meta_path = new_full_path.with_extension("meta.json");
                    std::fs::rename(&old_meta_path, &new_meta_path)?;
                }
                
                // Update the test in our config
                let new_relative_path = if new_full_path.starts_with("c-test/") {
                    new_full_path.strip_prefix("c-test/")?.to_path_buf()
                } else if let Ok(rel) = new_full_path.strip_prefix(std::env::current_dir()?.join("c-test")) {
                    rel.to_path_buf()
                } else {
                    // Try to make it relative to c-test
                    let path_str = new_full_path.to_string_lossy();
                    if let Some(idx) = path_str.find("c-test/") {
                        PathBuf::from(&path_str[idx + 7..])
                    } else {
                        new_full_path.clone()
                    }
                };
                
                // Update in test config
                if let Some(test) = self.test_config.tests.iter_mut()
                    .find(|t| t.file == *old_file) {
                    test.file = new_relative_path.clone();
                }
                
                // Update in orphan tests if applicable
                if let Some(test) = self.orphan_tests.iter_mut()
                    .find(|t| t.file == *old_file) {
                    test.file = new_relative_path;
                }
                
                // Rebuild categories
                self.categories = CategoryView::from_tests(
                    &self.test_config.tests,
                    &self.test_config.known_failures,
                    &self.orphan_tests
                );
                
                self.append_output(&format!("Test moved to category: {target_category}\n"));
            }
            
            // Clear move state
            self.editing_test_file = None;
            self.move_target_category.clear();
            self.mode = AppMode::Normal;
            self.show_categories = false;
        }
        
        Ok(())
    }
    
    pub fn cancel_move(&mut self) {
        self.editing_test_file = None;
        self.move_target_category.clear();
        self.mode = AppMode::Normal;
        self.show_categories = false;
    }
    
    pub fn start_create_test(&mut self) {
        // Clear the input fields
        self.new_test_name.clear();
        self.new_test_description = "New test".to_string();
        self.new_test_focused_field = false; // Start with name field focused
        self.mode = AppMode::CreatingTest;
    }
    
    pub fn save_new_test(&mut self) -> anyhow::Result<()> {
        if self.new_test_name.is_empty() {
            return Err(anyhow::anyhow!("Test name cannot be empty"));
        }
        
        // Get the current category to determine where to create the test
        let category = self.get_current_category_name()
            .unwrap_or_else(|| "Uncategorized".to_string());
        let dir = self.get_directory_for_category(&category)?;
        
        // Create the full path for the new test
        let test_name_with_ext = format!("{}.c", self.new_test_name);
        let test_path = PathBuf::from("c-test").join(&dir).join(&test_name_with_ext);
        
        // Check if test already exists
        if test_path.exists() {
            return Err(anyhow::anyhow!("A test with this name already exists"));
        }
        
        // Ensure directory exists
        if let Some(parent) = test_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        
        // Read template file
        let template_path = PathBuf::from("c-test/test-template.txt");
        if !template_path.exists() {
            return Err(anyhow::anyhow!("Template file c-test/test-template.txt not found"));
        }
        let template_content = std::fs::read_to_string(&template_path)?;
        
        // Replace placeholders in template
        let content = template_content
            .replace("{{TEST_NAME}}", &self.new_test_name)
            .replace("{{DESCRIPTION}}", &self.new_test_description);
        
        // Write the test file
        std::fs::write(&test_path, content)?;
        
        // Create metadata file
        let meta_path = test_path.with_extension("meta.json");
        let test_type = if test_path.extension() == Some(std::ffi::OsStr::new("bfm")) {
            TestType::Bfm
        } else {
            TestType::C
        };
        let metadata = crate::config::TestMetadata {
            expected: Some("Y\n".to_string()), // Default expected output from template (test passes)
            use_runtime: test_type == TestType::C,
            description: Some(self.new_test_description.clone()),
            known_failure: false,
            category: None,
            skipped: false,
            test_type,
            timeout_secs: None,
        };
        
        let meta_content = serde_json::to_string_pretty(&metadata)?;
        std::fs::write(&meta_path, meta_content)?;
        
        // Make the path relative for adding to config
        let relative_path = if test_path.starts_with("c-test/") {
            test_path.strip_prefix("c-test/")?.to_path_buf()
        } else if let Ok(rel) = test_path.strip_prefix(std::env::current_dir()?.join("c-test")) {
            rel.to_path_buf()
        } else {
            // Try to make it relative to c-test
            let path_str = test_path.to_string_lossy();
            if let Some(idx) = path_str.find("c-test/") {
                PathBuf::from(&path_str[idx + 7..])
            } else {
                test_path.clone()
            }
        };
        
        // Add to test config in alphabetically sorted position
        let new_test = TestCase {
            file: relative_path.clone(),
            expected: metadata.expected,
            use_runtime: metadata.use_runtime,
            description: metadata.description,
            skipped: metadata.skipped,
            test_type: metadata.test_type,
            timeout_secs: None,
        };
        
        // Find the correct position to insert (alphabetically by file path)
        let insert_pos = self.test_config.tests
            .iter()
            .position(|t| t.file > new_test.file)
            .unwrap_or(self.test_config.tests.len());
        
        self.test_config.tests.insert(insert_pos, new_test);
        
        // Rebuild categories
        self.categories = CategoryView::from_tests(
            &self.test_config.tests,
            &self.test_config.known_failures,
            &self.orphan_tests
        );
        
        // Jump to the new test
        let new_test = TestCase {
            file: relative_path,
            expected: Some("Y\n".to_string()),
            use_runtime: metadata.use_runtime,
            description: Some(self.new_test_description.clone()),
            skipped: false,
            test_type: metadata.test_type,
            timeout_secs: None,
        };
        self.jump_to_test(&new_test);
        
        // Clear input and return to normal mode
        self.new_test_name.clear();
        self.new_test_description.clear();
        self.mode = AppMode::Normal;
        
        self.append_output(&format!("Created new test: {test_name_with_ext}\n"));
        Ok(())
    }
    
    pub fn cancel_create_test(&mut self) {
        self.new_test_name.clear();
        self.new_test_description.clear();
        self.new_test_focused_field = false;
        self.mode = AppMode::Normal;
    }
    
    fn get_directory_for_category(&self, category: &str) -> anyhow::Result<String> {
        // Map category names to directory paths
        match category {
            "Runtime" => Ok("tests-runtime".to_string()),
            "Examples" => Ok("examples".to_string()),
            "Known Failures" => Ok("known-failures".to_string()),
            "Uncategorized" => Ok("tests".to_string()),
            cat if cat.starts_with("Known Failures › ") => {
                let subdir = cat.strip_prefix("Known Failures › ")
                    .unwrap_or("")
                    .to_lowercase()
                    .replace(' ', "-");
                Ok(format!("known-failures/{subdir}"))
            },
            cat if cat.starts_with("Orphan › ") => {
                // Extract the original category from orphan category
                let original = cat.strip_prefix("Orphan › ").unwrap_or("tests");
                self.get_directory_for_category(original)
            },
            cat => {
                // For nested categories like "Foo › Bar"
                let parts: Vec<&str> = cat.split(" › ").collect();
                let path = parts.join("/").to_lowercase().replace(' ', "-");
                Ok(format!("tests/{path}"))
            }
        }
    }

    // Tree navigation helpers for AST viewer
    pub fn ast_move_up(&mut self) {
        if self.ast_selected_path.is_empty() {
            return;
        }
        
        // Find the previous sibling or parent
        let lines = self.get_ast_lines();
        let current_idx = lines.iter().position(|(_, path, _)| path == &self.ast_selected_path);
        
        if let Some(idx) = current_idx {
            if idx > 0 {
                self.ast_selected_path = lines[idx - 1].1.clone();
                self.ensure_ast_selection_visible(&lines);
            }
        }
    }
    
    pub fn ast_move_down(&mut self) {
        let lines = self.get_ast_lines();
        let current_idx = lines.iter().position(|(_, path, _)| path == &self.ast_selected_path);
        
        if let Some(idx) = current_idx {
            if idx + 1 < lines.len() {
                self.ast_selected_path = lines[idx + 1].1.clone();
                self.ensure_ast_selection_visible(&lines);
            }
        } else if !lines.is_empty() {
            // If no selection, select first item
            self.ast_selected_path = lines[0].1.clone();
            self.ast_scroll = 0;
        }
    }
    
    pub fn ast_toggle_current(&mut self) {
        if self.ast_expanded_nodes.contains(&self.ast_selected_path) {
            self.ast_expanded_nodes.remove(&self.ast_selected_path);
        } else {
            self.ast_expanded_nodes.insert(self.ast_selected_path.clone());
        }
    }
    
    pub fn ast_expand_current(&mut self) {
        self.ast_expanded_nodes.insert(self.ast_selected_path.clone());
    }
    
    pub fn ast_collapse_current(&mut self) {
        self.ast_expanded_nodes.remove(&self.ast_selected_path);
    }
    
    pub fn ast_expand_all(&mut self) {
        let lines = self.get_ast_lines();
        for (_, path, _) in lines {
            if !path.is_empty() {
                self.ast_expanded_nodes.insert(path);
            }
        }
    }
    
    pub fn ast_collapse_all(&mut self) {
        self.ast_expanded_nodes.clear();
    }
    
    fn get_ast_lines(&self) -> Vec<(Vec<ratatui::text::Span<'static>>, Vec<usize>, bool)> {
        if let Some(test_name) = self.get_selected_test_name() {
            let ast_path = self.tools.build_dir.join(format!("{}.pp.ast.json", test_name));
            if ast_path.exists() {
                if let Ok(content) = std::fs::read_to_string(&ast_path) {
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&content) {
                        let tree = crate::tui::ui::trace_viewer::TreeNode::from_json(
                            &json, 
                            "AST Root".to_string(), 
                            crate::tui::ui::trace_viewer::NodeType::AstNode,
                            true
                        );
                        return tree.flatten(0, &self.ast_selected_path, &self.ast_expanded_nodes);
                    }
                }
            }
        }
        Vec::new()
    }
    
    fn ensure_ast_selection_visible(&mut self, lines: &[(Vec<ratatui::text::Span<'static>>, Vec<usize>, bool)]) {
        if let Some(idx) = lines.iter().position(|(_, path, _)| path == &self.ast_selected_path) {
            const VISIBLE_LINES: usize = 20; // Approximate visible lines
            
            if idx < self.ast_scroll {
                self.ast_scroll = idx;
            } else if idx >= self.ast_scroll + VISIBLE_LINES {
                self.ast_scroll = idx.saturating_sub(VISIBLE_LINES - 1);
            }
        }
    }
    
    // Similar helpers for TypedAST
    pub fn typed_ast_move_up(&mut self) {
        if self.typed_ast_selected_path.is_empty() {
            return;
        }
        
        let lines = self.get_typed_ast_lines();
        let current_idx = lines.iter().position(|(_, path, _)| path == &self.typed_ast_selected_path);
        
        if let Some(idx) = current_idx {
            if idx > 0 {
                self.typed_ast_selected_path = lines[idx - 1].1.clone();
                self.ensure_typed_ast_selection_visible(&lines);
            }
        }
    }
    
    pub fn typed_ast_move_down(&mut self) {
        let lines = self.get_typed_ast_lines();
        let current_idx = lines.iter().position(|(_, path, _)| path == &self.typed_ast_selected_path);
        
        if let Some(idx) = current_idx {
            if idx + 1 < lines.len() {
                self.typed_ast_selected_path = lines[idx + 1].1.clone();
                self.ensure_typed_ast_selection_visible(&lines);
            }
        } else if !lines.is_empty() {
            self.typed_ast_selected_path = lines[0].1.clone();
            self.typed_ast_scroll = 0;
        }
    }
    
    pub fn typed_ast_toggle_current(&mut self) {
        if self.typed_ast_expanded_nodes.contains(&self.typed_ast_selected_path) {
            self.typed_ast_expanded_nodes.remove(&self.typed_ast_selected_path);
        } else {
            self.typed_ast_expanded_nodes.insert(self.typed_ast_selected_path.clone());
        }
    }
    
    pub fn typed_ast_expand_current(&mut self) {
        self.typed_ast_expanded_nodes.insert(self.typed_ast_selected_path.clone());
    }
    
    pub fn typed_ast_collapse_current(&mut self) {
        self.typed_ast_expanded_nodes.remove(&self.typed_ast_selected_path);
    }
    
    pub fn typed_ast_expand_all(&mut self) {
        let lines = self.get_typed_ast_lines();
        for (_, path, _) in lines {
            if !path.is_empty() {
                self.typed_ast_expanded_nodes.insert(path);
            }
        }
    }
    
    pub fn typed_ast_collapse_all(&mut self) {
        self.typed_ast_expanded_nodes.clear();
    }
    
    pub fn expand_all_categories(&mut self) {
        for category in self.categories.values_mut() {
            category.expanded = true;
        }
    }
    
    pub fn collapse_all_categories(&mut self) {
        for category in self.categories.values_mut() {
            category.expanded = false;
        }
    }
    
    pub fn handle_test_list_click(&mut self, clicked_index: usize) {
        // Early return if the clicked index is beyond visible items
        let total_visible = self.get_total_visible_items();
        if clicked_index >= total_visible {
            return;
        }
        
        // Find what was clicked - could be a category header or a test
        let mut current_index = 0;
        
        // We need to clone the category names to avoid borrowing issues
        let category_names: Vec<String> = self.categories.keys().cloned().collect();
        
        for category_name in category_names {
            // Check if this is the category header
            if current_index == clicked_index {
                // Toggle category expansion
                if let Some(category) = self.categories.get_mut(&category_name) {
                    category.expanded = !category.expanded;
                }
                return;
            }
            current_index += 1;
            
            // If category is expanded, check tests
            if let Some(category) = self.categories.get(&category_name) {
                if category.expanded {
                    if clicked_index < current_index + category.tests.len() {
                        // Clicked on a test - select it
                        self.selected_item = clicked_index;
                        self.ensure_selection_visible();
                        return;
                    }
                    current_index += category.tests.len();
                }
            }
        }
    }
    
    pub fn handle_ast_click(&mut self, clicked_line: usize) {
        // Get the AST lines to find which node was clicked
        let lines = self.get_ast_lines();
        if clicked_line < lines.len() {
            let (_, path, _) = &lines[clicked_line];
            // Select the clicked node
            self.ast_selected_path = path.clone();
            // Toggle expansion of the clicked node
            self.ast_toggle_current();
        }
    }
    
    pub fn handle_typed_ast_click(&mut self, clicked_line: usize) {
        // Get the TypedAST lines to find which node was clicked
        let lines = self.get_typed_ast_lines();
        if clicked_line < lines.len() {
            let (_, path, _) = &lines[clicked_line];
            // Select the clicked node
            self.typed_ast_selected_path = path.clone();
            // Toggle expansion of the clicked node
            self.typed_ast_toggle_current();
        }
    }
    
    fn get_typed_ast_lines(&self) -> Vec<(Vec<ratatui::text::Span<'static>>, Vec<usize>, bool)> {
        if let Some(test_name) = self.get_selected_test_name() {
            let tast_path = self.tools.build_dir.join(format!("{}.pp.tast.json", test_name));
            if tast_path.exists() {
                if let Ok(content) = std::fs::read_to_string(&tast_path) {
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&content) {
                        let tree = crate::tui::ui::trace_viewer::TreeNode::from_json(
                            &json, 
                            "Typed AST".to_string(), 
                            crate::tui::ui::trace_viewer::NodeType::AstNode,
                            true
                        );
                        return tree.flatten(0, &self.typed_ast_selected_path, &self.typed_ast_expanded_nodes);
                    }
                }
            }
        }
        Vec::new()
    }
    
    fn ensure_typed_ast_selection_visible(&mut self, lines: &[(Vec<ratatui::text::Span<'static>>, Vec<usize>, bool)]) {
        if let Some(idx) = lines.iter().position(|(_, path, _)| path == &self.typed_ast_selected_path) {
            const VISIBLE_LINES: usize = 20;
            
            if idx < self.typed_ast_scroll {
                self.typed_ast_scroll = idx;
            } else if idx >= self.typed_ast_scroll + VISIBLE_LINES {
                self.typed_ast_scroll = idx.saturating_sub(VISIBLE_LINES - 1);
            }
        }
    }
    
    pub fn save_metadata(&mut self) -> anyhow::Result<()> {
        if let Some(test_file) = &self.metadata_input.test_file {
            // Create metadata
            let test_type = if test_file.extension() == Some(std::ffi::OsStr::new("bfm")) {
                TestType::Bfm
            } else {
                TestType::C
            };
            let metadata = crate::config::TestMetadata {
                expected: if self.metadata_input.is_known_failure || self.metadata_input.expected_output.is_empty() {
                    None  // Don't set expected if it's empty or a known failure
                } else {
                    Some(self.metadata_input.expected_output.clone())
                },
                use_runtime: test_type == TestType::C && self.metadata_input.use_runtime,
                description: if self.metadata_input.description.is_empty() {
                    None
                } else {
                    Some(self.metadata_input.description.clone())
                },
                known_failure: self.metadata_input.is_known_failure,
                category: None,
                skipped: false,
                test_type,
                timeout_secs: None,
            };

            // Save the metadata file
            let full_path = if test_file.is_relative() && !test_file.starts_with("c-test") {
                PathBuf::from("c-test").join(test_file)
            } else {
                test_file.clone()
            };
            
            let meta_path = full_path.with_extension("meta.json");
            let content = serde_json::to_string_pretty(&metadata)?;
            std::fs::write(meta_path, content)?;

            // Remove from orphan list and add to appropriate category
            if let Some(idx) = self.orphan_tests.iter().position(|t| t.file == *test_file) {
                let orphan = self.orphan_tests.remove(idx);
                
                // Add to test config
                if self.metadata_input.is_known_failure {
                    self.test_config.known_failures.push(KnownFailure {
                        file: orphan.file,
                        description: metadata.description,
                    });
                } else {
                    self.test_config.tests.push(TestCase {
                        file: orphan.file,
                        expected: metadata.expected.clone(),  // Use the properly set expected value
                        use_runtime: metadata.use_runtime,
                        description: metadata.description,
                        skipped: metadata.skipped,
                        test_type: metadata.test_type,
                        timeout_secs: metadata.timeout_secs,
                    });
                }
                
                // Rebuild categories
                self.categories = CategoryView::from_tests(
                    &self.test_config.tests,
                    &self.test_config.known_failures,
                    &self.orphan_tests
                );
            }

            // Clear metadata input and return to normal mode
            self.metadata_input = MetadataInput::default();
            self.mode = AppMode::Normal;
        }
        
        Ok(())
    }
}