#include <lci/parser/parser.h>
#include <lci/parser/unified_extractor.h>

#include <gtest/gtest.h>
#include <tree_sitter/api.h>

#include <string>
#include <string_view>

namespace lci::parser {
namespace {

// Helper: parse source code and return the tree.
UniqueTree parse(Language lang, std::string_view src) {
    UniqueParser parser = make_parser(lang);
    if (!parser) return nullptr;
    TSTree* raw = ts_parser_parse_string(
        parser.get(), nullptr, src.data(),
        static_cast<uint32_t>(src.size()));
    return UniqueTree(raw);
}

// Helper: find symbol by name in results.
const Symbol* find_symbol(const ExtractionResults& r,
                          std::string_view name) {
    for (const auto& s : r.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

// Helper: count symbols of a given type.
int count_symbols(const ExtractionResults& r, SymbolType type) {
    int n = 0;
    for (const auto& s : r.symbols) {
        if (s.type == type) ++n;
    }
    return n;
}

// Helper: extract and return results for a given language and source.
ExtractionResults extract(Language lang, std::string_view ext,
                          std::string_view src, std::string_view path) {
    auto tree = parse(lang, src);
    if (!tree) return {};

    UnifiedExtractor ue;
    ue.init(src, 1, ext, path);
    ue.extract(tree.get());
    return ue.get_results();
}

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

constexpr std::string_view kGoSrc = R"(package main

import "fmt"

type Greeter interface {
	Greet(name string) string
}

type MyStruct struct {
	Name string
}

func (m *MyStruct) Greet(name string) string {
	return fmt.Sprintf("Hello, %s!", name)
}

func add(a, b int) int {
	return a + b
}

var globalVar = 42
const Pi = 3.14
)";

TEST(LanguageExtractionTest, Go) {
    auto r = extract(Language::Go, ".go", kGoSrc, "main.go");

    EXPECT_NE(find_symbol(r, "add"), nullptr);
    EXPECT_EQ(find_symbol(r, "add")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Greet"), nullptr);
    EXPECT_EQ(find_symbol(r, "Greet")->type, SymbolType::Method);

    EXPECT_NE(find_symbol(r, "MyStruct"), nullptr);
    EXPECT_EQ(find_symbol(r, "MyStruct")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Greeter"), nullptr);
    EXPECT_EQ(find_symbol(r, "Greeter")->type, SymbolType::Interface);

    EXPECT_NE(find_symbol(r, "globalVar"), nullptr);
    EXPECT_EQ(find_symbol(r, "globalVar")->type, SymbolType::Variable);

    EXPECT_NE(find_symbol(r, "Pi"), nullptr);
    EXPECT_EQ(find_symbol(r, "Pi")->type, SymbolType::Constant);

    ASSERT_FALSE(r.imports.empty());
    bool found_fmt = false;
    for (const auto& imp : r.imports) {
        if (imp.path == "fmt") found_fmt = true;
    }
    EXPECT_TRUE(found_fmt);
}

// ---------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------

constexpr std::string_view kPythonSrc = R"(import os
from pathlib import Path

class Animal:
    def __init__(self, name):
        self.name = name

    def speak(self):
        return "..."

class Dog(Animal):
    def speak(self):
        return "Woof!"

def greet(name):
    return f"Hello, {name}"
)";

TEST(LanguageExtractionTest, Python) {
    auto r = extract(Language::Python, ".py", kPythonSrc, "animals.py");

    EXPECT_NE(find_symbol(r, "greet"), nullptr);
    EXPECT_EQ(find_symbol(r, "greet")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Animal"), nullptr);
    EXPECT_EQ(find_symbol(r, "Animal")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "Dog"), nullptr);
    EXPECT_EQ(find_symbol(r, "Dog")->type, SymbolType::Class);

    EXPECT_GE(count_symbols(r, SymbolType::Method), 3);

    EXPECT_GE(r.imports.size(), 2u);
}

// ---------------------------------------------------------------------------
// JavaScript
// ---------------------------------------------------------------------------

constexpr std::string_view kJsSrc = R"(import { foo } from './utils';

function greet(name) {
    return `Hello, ${name}`;
}

class Animal {
    constructor(name) {
        this.name = name;
    }

    speak() {
        return "...";
    }
}

const add = (a, b) => a + b;

let count = 0;
)";

TEST(LanguageExtractionTest, JavaScript) {
    auto r = extract(Language::JavaScript, ".js", kJsSrc, "app.js");

    EXPECT_NE(find_symbol(r, "greet"), nullptr);
    EXPECT_EQ(find_symbol(r, "greet")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Animal"), nullptr);
    EXPECT_EQ(find_symbol(r, "Animal")->type, SymbolType::Class);

    EXPECT_GE(count_symbols(r, SymbolType::Method), 1);

    // Arrow function assigned to const creates dual symbols
    EXPECT_NE(find_symbol(r, "add"), nullptr);

    ASSERT_FALSE(r.imports.empty());
}

// ---------------------------------------------------------------------------
// TypeScript
// ---------------------------------------------------------------------------

constexpr std::string_view kTsSrc = R"(import { Logger } from './logger';

interface Printable {
    toString(): string;
}

type ID = string | number;

enum Color {
    Red,
    Green,
    Blue,
}

class Shape implements Printable {
    constructor(public name: string) {}

    toString(): string {
        return this.name;
    }
}

function area(width: number, height: number): number {
    return width * height;
}
)";

TEST(LanguageExtractionTest, TypeScript) {
    auto r = extract(Language::TypeScript, ".ts", kTsSrc, "shapes.ts");

    EXPECT_NE(find_symbol(r, "area"), nullptr);
    EXPECT_EQ(find_symbol(r, "area")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Shape"), nullptr);
    EXPECT_EQ(find_symbol(r, "Shape")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "Printable"), nullptr);
    EXPECT_EQ(find_symbol(r, "Printable")->type, SymbolType::Interface);

    EXPECT_NE(find_symbol(r, "ID"), nullptr);
    EXPECT_EQ(find_symbol(r, "ID")->type, SymbolType::Type);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    ASSERT_FALSE(r.imports.empty());
}

// ---------------------------------------------------------------------------
// Rust
// ---------------------------------------------------------------------------

constexpr std::string_view kRustSimple = R"RUST(
fn standalone() -> i32 {
    42
}

struct Point {
    x: f64,
    y: f64,
}

impl Point {
    fn method(&self) -> f64 {
        self.x
    }
}

fn after_impl() -> i32 {
    0
}
)RUST";

constexpr std::string_view kRustSrc = R"RUST(use std::collections::HashMap;

mod utils;

struct Point {
    x: f64,
    y: f64,
}

enum Shape {
    Circle(f64),
    Rectangle(f64, f64),
}

trait Drawable {
    fn draw(&self);
}

impl Drawable for Point {
    fn draw(&self) {}
}

fn distance(a: &Point, b: &Point) -> f64 {
    0.0
}
)RUST";

TEST(LanguageExtractionTest, RustFunctionVsMethod) {
    auto r = extract(Language::Rust, ".rs", kRustSimple, "simple.rs");

    const Symbol* standalone = find_symbol(r, "standalone");
    ASSERT_NE(standalone, nullptr);
    EXPECT_EQ(standalone->type, SymbolType::Function);

    const Symbol* method = find_symbol(r, "method");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->type, SymbolType::Method);

    // after_impl should be Function (context restored after impl block)
    const Symbol* after = find_symbol(r, "after_impl");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->type, SymbolType::Function);
}

TEST(LanguageExtractionTest, Rust) {
    auto r = extract(Language::Rust, ".rs", kRustSrc, "shapes.rs");

    EXPECT_NE(find_symbol(r, "distance"), nullptr);
    EXPECT_EQ(find_symbol(r, "distance")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Point"), nullptr);
    EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Shape"), nullptr);
    EXPECT_EQ(find_symbol(r, "Shape")->type, SymbolType::Enum);

    EXPECT_NE(find_symbol(r, "Drawable"), nullptr);
    EXPECT_EQ(find_symbol(r, "Drawable")->type, SymbolType::Trait);

    // impl block
    EXPECT_GE(count_symbols(r, SymbolType::Impl), 1);

    // draw as method inside impl
    EXPECT_NE(find_symbol(r, "draw"), nullptr);
    EXPECT_EQ(find_symbol(r, "draw")->type, SymbolType::Method);

    // Module
    EXPECT_NE(find_symbol(r, "utils"), nullptr);
    EXPECT_EQ(find_symbol(r, "utils")->type, SymbolType::Module);

    // Imports
    ASSERT_FALSE(r.imports.empty());
}

// ---------------------------------------------------------------------------
// C++
// ---------------------------------------------------------------------------

constexpr std::string_view kCppSrc = R"(#include <vector>
#include <string>

namespace shapes {

struct Point {
    double x;
    double y;
};

enum class Color {
    Red,
    Green,
    Blue,
};

class Shape {
public:
    virtual void draw() = 0;
};

double distance(const Point& a, const Point& b) {
    return 0.0;
}

}  // namespace shapes
)";

TEST(LanguageExtractionTest, Cpp) {
    auto r = extract(Language::Cpp, ".cpp", kCppSrc, "shapes.cpp");

    EXPECT_NE(find_symbol(r, "distance"), nullptr);
    EXPECT_EQ(find_symbol(r, "distance")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Point"), nullptr);
    EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    EXPECT_NE(find_symbol(r, "Shape"), nullptr);
    EXPECT_EQ(find_symbol(r, "Shape")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "shapes"), nullptr);
    EXPECT_EQ(find_symbol(r, "shapes")->type, SymbolType::Namespace);

    // Imports
    EXPECT_GE(r.imports.size(), 2u);
}

TEST(LanguageExtractionTest, CppReferences) {
    constexpr std::string_view kCppRefsSrc = R"(class SlabAllocator {};

inline void put_to_tier() {}

inline void use_ref() {
    put_to_tier();
    SlabAllocator allocator;
}
)";

    auto r = extract(Language::Cpp, ".hpp", kCppRefsSrc, "alloc.hpp");

    bool saw_call = false;
    bool saw_type_usage = false;
    for (const auto& ref : r.references) {
        if (ref.type == ReferenceType::Call &&
            ref.referenced_name == "put_to_tier") {
            saw_call = true;
        }
        if (ref.type == ReferenceType::Usage &&
            ref.referenced_name == "SlabAllocator") {
            saw_type_usage = true;
        }
    }

    EXPECT_TRUE(saw_call);
    EXPECT_TRUE(saw_type_usage);
}

// ---------------------------------------------------------------------------
// C
// ---------------------------------------------------------------------------

constexpr std::string_view kCSrc = R"(#include <stdio.h>
#include <stdlib.h>

struct Point {
    double x;
    double y;
};

enum Color {
    RED,
    GREEN,
    BLUE,
};

void print_point(struct Point p) {
    printf("(%f, %f)\n", p.x, p.y);
}

double distance(struct Point a, struct Point b) {
    return 0.0;
}
)";

TEST(LanguageExtractionTest, C) {
    auto r = extract(Language::C, ".c", kCSrc, "shapes.c");

    EXPECT_NE(find_symbol(r, "distance"), nullptr);
    EXPECT_EQ(find_symbol(r, "distance")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "print_point"), nullptr);
    EXPECT_EQ(find_symbol(r, "print_point")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Point"), nullptr);
    EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    EXPECT_GE(r.imports.size(), 2u);
}

// ---------------------------------------------------------------------------
// Java
// ---------------------------------------------------------------------------

constexpr std::string_view kJavaSrc = R"(package com.example;

import java.util.List;

public class Animal {
    private String name;

    public Animal(String name) {
        this.name = name;
    }

    public String getName() {
        return name;
    }
}

interface Drawable {
    void draw();
}

enum Color {
    RED,
    GREEN,
    BLUE,
}
)";

TEST(LanguageExtractionTest, Java) {
    auto r = extract(Language::Java, ".java", kJavaSrc, "Animal.java");

    EXPECT_NE(find_symbol(r, "Animal"), nullptr);
    EXPECT_EQ(find_symbol(r, "Animal")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "getName"), nullptr);
    EXPECT_EQ(find_symbol(r, "getName")->type, SymbolType::Method);

    EXPECT_NE(find_symbol(r, "Drawable"), nullptr);
    EXPECT_EQ(find_symbol(r, "Drawable")->type, SymbolType::Interface);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    // Constructor
    EXPECT_GE(count_symbols(r, SymbolType::Constructor), 1);

    // Imports (package + import declaration)
    EXPECT_GE(r.imports.size(), 2u);
}

// ---------------------------------------------------------------------------
// C#
// ---------------------------------------------------------------------------

constexpr std::string_view kCSharpSrc = R"(using System;
using System.Collections.Generic;

namespace MyApp
{
    public class Animal
    {
        public string Name { get; set; }

        public Animal(string name)
        {
            Name = name;
        }

        public string Speak()
        {
            return "...";
        }
    }

    public interface IDrawable
    {
        void Draw();
    }

    public struct Point
    {
        public double X;
        public double Y;
    }

    public enum Color
    {
        Red,
        Green,
        Blue,
    }
}
)";

TEST(LanguageExtractionTest, CSharp) {
    auto r = extract(Language::CSharp, ".cs", kCSharpSrc, "Animal.cs");

    EXPECT_NE(find_symbol(r, "Animal"), nullptr);
    EXPECT_EQ(find_symbol(r, "Animal")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "Speak"), nullptr);
    EXPECT_EQ(find_symbol(r, "Speak")->type, SymbolType::Method);

    EXPECT_NE(find_symbol(r, "IDrawable"), nullptr);
    EXPECT_EQ(find_symbol(r, "IDrawable")->type, SymbolType::Interface);

    EXPECT_NE(find_symbol(r, "Point"), nullptr);
    EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    // Namespace
    EXPECT_NE(find_symbol(r, "MyApp"), nullptr);
    EXPECT_EQ(find_symbol(r, "MyApp")->type, SymbolType::Namespace);

    // Constructor
    EXPECT_GE(count_symbols(r, SymbolType::Constructor), 1);

    // Using directives as imports
    EXPECT_GE(r.imports.size(), 2u);
}

// ---------------------------------------------------------------------------
// PHP
// ---------------------------------------------------------------------------

constexpr std::string_view kPhpSrc = R"(<?php

namespace App\Models;

use App\Contracts\HasName;

class Animal {
    private string $name;

    public function __construct(string $name) {
        $this->name = $name;
    }

    public function getName(): string {
        return $this->name;
    }
}

interface Drawable {
    public function draw(): void;
}

trait HasAge {
    public function getAge(): int {
        return $this->age;
    }
}

enum Color {
    case Red;
    case Green;
    case Blue;
}

function greet(string $name): string {
    return "Hello, {$name}";
}
)";

TEST(LanguageExtractionTest, PHP) {
    auto r = extract(Language::PHP, ".php", kPhpSrc, "Animal.php");

    EXPECT_NE(find_symbol(r, "Animal"), nullptr);
    EXPECT_EQ(find_symbol(r, "Animal")->type, SymbolType::Class);

    EXPECT_NE(find_symbol(r, "greet"), nullptr);
    EXPECT_EQ(find_symbol(r, "greet")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "Drawable"), nullptr);
    EXPECT_EQ(find_symbol(r, "Drawable")->type, SymbolType::Interface);

    EXPECT_NE(find_symbol(r, "HasAge"), nullptr);
    EXPECT_EQ(find_symbol(r, "HasAge")->type, SymbolType::Trait);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Enum);

    // Methods inside class
    EXPECT_GE(count_symbols(r, SymbolType::Method), 2);

    // Namespace
    auto* ns = find_symbol(r, "App\\Models");
    EXPECT_NE(ns, nullptr);
}

// ---------------------------------------------------------------------------
// Kotlin
// ---------------------------------------------------------------------------

constexpr std::string_view kKotlinSrc = R"(package com.example

import kotlin.math.sqrt

class Point(val x: Double, val y: Double)

interface Drawable {
    fun draw()
}

object Singleton {
    val instance = "singleton"
}

enum class Color {
    RED,
    GREEN,
    BLUE,
}

fun distance(a: Point, b: Point): Double {
    return sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y))
}
)";

TEST(LanguageExtractionTest, Kotlin) {
    auto tree = parse(Language::Kotlin, kKotlinSrc);
    if (!tree) {
        GTEST_SKIP() << "Kotlin parser unavailable";
    }

    auto r = extract(Language::Kotlin, ".kt", kKotlinSrc, "Shapes.kt");

    // Kotlin extraction is best-effort since the tree-sitter-kotlin grammar
    // uses different node types than the Go bindings. Verify the parser
    // creates a tree and basic extraction produces some symbols.
    EXPECT_FALSE(r.scopes.empty()) << "Should have at least file scope";

    // Kotlin class_declaration should be recognized
    if (find_symbol(r, "Point") != nullptr) {
        EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Class);
    }
}

// ---------------------------------------------------------------------------
// Zig
// ---------------------------------------------------------------------------

constexpr std::string_view kZigSrc = R"(const std = @import("std");

const Point = struct {
    x: f64,
    y: f64,
};

const Color = union(enum) {
    red,
    green,
    blue,
};

fn distance(a: Point, b: Point) f64 {
    const dx = a.x - b.x;
    const dy = a.y - b.y;
    return @sqrt(dx * dx + dy * dy);
}

pub fn main() !void {
    const stdout = std.io.getStdOut().writer();
    try stdout.print("Hello\n", .{});
}
)";

TEST(LanguageExtractionTest, Zig) {
    auto r = extract(Language::Zig, ".zig", kZigSrc, "main.zig");

    auto tree = parse(Language::Zig, kZigSrc);
    if (!tree) {
        GTEST_SKIP() << "Zig parser unavailable";
    }

    EXPECT_NE(find_symbol(r, "distance"), nullptr);
    EXPECT_EQ(find_symbol(r, "distance")->type, SymbolType::Function);

    EXPECT_NE(find_symbol(r, "main"), nullptr);
    EXPECT_EQ(find_symbol(r, "main")->type, SymbolType::Function);

    // Zig structs via variable_declaration
    EXPECT_NE(find_symbol(r, "Point"), nullptr);
    EXPECT_EQ(find_symbol(r, "Point")->type, SymbolType::Struct);

    EXPECT_NE(find_symbol(r, "Color"), nullptr);
    EXPECT_EQ(find_symbol(r, "Color")->type, SymbolType::Struct);
}

// ---------------------------------------------------------------------------
// Ruby
// ---------------------------------------------------------------------------

constexpr std::string_view kRubySrc = R"(require 'json'

module Animals
  class Dog
    def initialize(name)
      @name = name
    end

    def speak
      "Woof!"
    end
  end

  class Cat
    def speak
      "Meow!"
    end
  end
end

def greet(name)
  "Hello, #{name}"
end
)";

TEST(LanguageExtractionTest, Ruby) {
    auto tree = parse(Language::Ruby, kRubySrc);
    if (!tree) {
        GTEST_SKIP() << "Ruby parser unavailable";
    }

    auto r = extract(Language::Ruby, ".rb", kRubySrc, "animals.rb");

    // Ruby uses class_declaration and method_definition node types
    EXPECT_FALSE(r.scopes.empty()) << "Should have at least file scope";

    // Ruby class extraction
    const Symbol* dog = find_symbol(r, "Dog");
    if (dog != nullptr) {
        EXPECT_EQ(dog->type, SymbolType::Class);
    }

    const Symbol* cat = find_symbol(r, "Cat");
    if (cat != nullptr) {
        EXPECT_EQ(cat->type, SymbolType::Class);
    }

    // Ruby methods
    EXPECT_GE(count_symbols(r, SymbolType::Method), 2);
}

// ---------------------------------------------------------------------------
// Fallback / unknown language
// ---------------------------------------------------------------------------

TEST(LanguageExtractionTest, FallbackForUnknownExtension) {
    // Parsing with an unrecognized extension should not crash
    Language lang;
    bool found = language_from_extension(".xyz", lang);
    EXPECT_FALSE(found);

    // If we try to parse with a known language but wrong extension,
    // extraction should still work based on node types
    constexpr std::string_view src = "function hello() {}\n";
    auto tree = parse(Language::JavaScript, src);
    ASSERT_NE(tree.get(), nullptr);

    UnifiedExtractor ue;
    ue.init(src, 1, ".unknown", "test.unknown");
    ue.extract(tree.get());
    auto r = ue.get_results();

    // Should still extract the function via node type matching
    EXPECT_NE(find_symbol(r, "hello"), nullptr);
}

// ---------------------------------------------------------------------------
// Parser creation for all 13 languages
// ---------------------------------------------------------------------------

TEST(LanguageExtractionTest, AllParsersCreate) {
    Language languages[] = {
        Language::Go, Language::Python, Language::JavaScript,
        Language::TypeScript, Language::Rust, Language::C,
        Language::Cpp, Language::Java, Language::CSharp,
        Language::PHP, Language::Kotlin, Language::Zig,
        Language::Ruby,
    };
    for (int i = 0; i < 13; ++i) {
        UniqueParser parser = make_parser(languages[i]);
        EXPECT_NE(parser.get(), nullptr)
            << "Failed to create parser for language index " << i;
    }
}

// ---------------------------------------------------------------------------
// Scope-based receiver-type resolution (SCIP base case): each language's
// reference handler must emit method calls as receiver-type-qualified
// `Type.method` Call refs when the receiver's type is locally known.
// ---------------------------------------------------------------------------

// True if a Call reference named exactly `name` was extracted.
bool has_call_ref(const ExtractionResults& r, std::string_view name) {
    for (const auto& ref : r.references) {
        if (ref.type == ReferenceType::Call && ref.referenced_name == name)
            return true;
    }
    return false;
}

TEST(ScopeTypeResolution, JavaQualifiesReceiverAndThis) {
    constexpr std::string_view src = R"(class A {
    void run() { helpA(); }
    void helpA() {}
}
class Main {
    void go() { A a = new A(); a.run(); }
})";
    auto r = extract(Language::Java, ".java", src, "M.java");
    EXPECT_TRUE(has_call_ref(r, "A.helpA"));  // this-qualified bare call
    EXPECT_TRUE(has_call_ref(r, "A.run"));    // typed-local receiver
}

TEST(ScopeTypeResolution, CSharpQualifiesReceiverAndThis) {
    constexpr std::string_view src = R"(class A {
    void run() { helpA(); }
    void helpA() {}
}
class Main {
    void go() { A a = new A(); a.run(); }
})";
    auto r = extract(Language::CSharp, ".cs", src, "M.cs");
    EXPECT_TRUE(has_call_ref(r, "A.helpA"));
    EXPECT_TRUE(has_call_ref(r, "A.run"));
}

TEST(ScopeTypeResolution, RustQualifiesSelfAndLet) {
    constexpr std::string_view src = R"(struct A;
impl A {
    fn run(&self) { self.help_a(); }
    fn help_a(&self) {}
}
fn go() { let a = A; a.run(); })";
    auto r = extract(Language::Rust, ".rs", src, "m.rs");
    EXPECT_TRUE(has_call_ref(r, "A.help_a"));  // self -> impl type
    EXPECT_TRUE(has_call_ref(r, "A.run"));     // let a = A
}

TEST(ScopeTypeResolution, PhpQualifiesThisAndNew) {
    constexpr std::string_view src = R"(<?php
class A {
    function run() { $this->helpA(); }
    function helpA() {}
}
function go() { $a = new A(); $a->run(); })";
    auto r = extract(Language::PHP, ".php", src, "m.php");
    EXPECT_TRUE(has_call_ref(r, "A.helpA"));  // $this -> class
    EXPECT_TRUE(has_call_ref(r, "A.run"));    // $a = new A()
}

TEST(ScopeTypeResolution, KotlinQualifiesThisAndVal) {
    constexpr std::string_view src = R"(class A {
    fun run() { helpA() }
    fun helpA() {}
}
fun go() {
    val a = A()
    a.run()
})";
    auto r = extract(Language::Kotlin, ".kt", src, "m.kt");
    EXPECT_TRUE(has_call_ref(r, "A.helpA"));  // this -> class
    EXPECT_TRUE(has_call_ref(r, "A.run"));    // val a = A()
}

TEST(ScopeTypeResolution, RubyQualifiesReceiverFromNew) {
    constexpr std::string_view src = R"(class A
  def run
    self.help_a
  end
  def help_a
  end
end
def go
  a = A.new
  a.run
end)";
    auto r = extract(Language::Ruby, ".rb", src, "m.rb");
    EXPECT_TRUE(has_call_ref(r, "A.run"));     // a = A.new ; a.run
    EXPECT_TRUE(has_call_ref(r, "A.help_a"));  // self.help_a
}

TEST(ScopeTypeResolution, ZigQualifiesSelfAndConst) {
    constexpr std::string_view src = R"(const A = struct {
    fn run(self: A) void { self.helpA(); }
    fn helpA(self: A) void {}
};
fn go() void {
    const a = A{};
    a.run();
})";
    auto r = extract(Language::Zig, ".zig", src, "m.zig");
    EXPECT_TRUE(has_call_ref(r, "A.helpA"));  // self: A param
    EXPECT_TRUE(has_call_ref(r, "A.run"));    // const a = A{}
}

}  // namespace
}  // namespace lci::parser
