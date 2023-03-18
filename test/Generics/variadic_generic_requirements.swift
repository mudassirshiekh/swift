// RUN: %target-typecheck-verify-swift -enable-experimental-feature VariadicGenerics

// REQUIRES: asserts

struct Conformance<each T: Equatable> {}

_ = Conformance<Int, String>.self  // ok
_ = Conformance<AnyObject, Character>.self  // expected-error {{type 'AnyObject' does not conform to protocol 'Equatable'}}

class Class {}
class OtherClass {}
class Subclass: Class {}

struct Superclass<each T: Class> {}  // expected-note {{requirement specified as 'T' : 'Class' [with each T = OtherClass]}}

_ = Superclass<Class, Subclass>.self  // ok
_ = Superclass<OtherClass>.self  // expected-error {{'Superclass' requires that 'OtherClass' inherit from 'Class'}}

struct Layout<each T: AnyObject> {}  // expected-note {{requirement specified as 'T' : 'AnyObject' [with each T = Int, String]}}

_ = Layout<Class, Subclass>.self  // ok
_ = Layout<Int, String>.self  // expected-error {{'Layout' requires that 'Int' be a class type}}

struct Outer<each T: Sequence> {
  struct Inner<each U: Sequence> where each T.Element == each U.Element {}
  // expected-note@-1 {{requirement specified as 'T.Element' == 'U.Element' [with each T = Array<Int>, Array<String>; each U = Set<String>, Set<Int>]}}
  // expected-note@-2 {{requirement specified as 'T.Element' == 'U.Element' [with each T = Array<Int>; each U = Set<Int>, Set<String>]}}

  struct InnerShape<each U: Sequence> where (repeat (each T, each U)): Any {}
  // expected-note@-1 {{same-shape requirement inferred between 'T' and 'U' [with each T = Array<Int>; each U = Set<Int>, Set<String>]}}

}

_ = Outer<Array<Int>, Array<String>>.Inner<Set<Int>, Set<String>>.self  // ok
_ = Outer<Array<Int>, Array<String>>.Inner<Set<String>, Set<Int>>.self  // expected-error {{'Outer<Array<Int>, Array<String>>.Inner' requires the types 'Pack{Int, String}' and 'Pack{String, Int}' be equivalent}}
_ = Outer<Array<Int>>.Inner<Set<Int>, Set<String>>.self  // expected-error {{'Outer<Array<Int>>.Inner' requires the types 'Pack{Int}' and 'Pack{Int, String}' be equivalent}}

_ = Outer<Array<Int>, Array<String>>.InnerShape<Set<String>, Set<Int>>.self  // ok
_ = Outer<Array<Int>>.InnerShape<Set<Int>, Set<String>>.self  // expected-error {{'Outer<Array<Int>>.InnerShape' requires the type packs 'Pack{Array<Int>}' and 'Pack{Set<Int>, Set<String>}' have the same shape}}