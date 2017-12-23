struct Foo;

void foo() {
  Foo* a;
}

/*
OUTPUT:
{
  "includes": [],
  "skipped_by_preprocessor": [],
  "types": [{
      "id": 0,
      "usr": "c:@S@Foo",
      "short_name": "",
      "detailed_name": "",
      "parents": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [0],
      "uses": ["1:8-1:11", "4:3-4:6"]
    }],
  "funcs": [{
      "id": 0,
      "is_operator": false,
      "usr": "c:@F@foo#",
      "short_name": "foo",
      "detailed_name": "void foo()",
      "hover": "void foo()",
      "declarations": [],
      "definition_spelling": "3:6-3:9",
      "definition_extent": "3:1-5:2",
      "base": [],
      "derived": [],
      "locals": [],
      "callers": [],
      "callees": []
    }],
  "vars": [{
      "id": 0,
      "usr": "c:function_local.cc@31@F@foo#@a",
      "short_name": "a",
      "detailed_name": "Foo * a",
      "definition_spelling": "4:8-4:9",
      "definition_extent": "4:3-4:9",
      "variable_type": 0,
      "is_local": true,
      "is_macro": false,
      "is_global": false,
      "is_member": false,
      "uses": ["4:8-4:9"]
    }]
}
*/
