void foo(int a, int b);

/*
OUTPUT:
{
  "includes": [],
  "skipped_by_preprocessor": [],
  "types": [],
  "funcs": [{
      "id": 0,
      "is_operator": false,
      "usr": "c:@F@foo#I#I#",
      "short_name": "foo",
      "detailed_name": "void foo(int a, int b)",
      "declarations": [{
          "spelling": "1:6-1:9",
          "extent": "1:1-1:23",
          "content": "void foo(int a, int b)",
          "param_spellings": ["1:14-1:15", "1:21-1:22"]
        }],
      "definition_spelling": "-1:-1--1:-1",
      "definition_extent": "-1:-1--1:-1",
      "base": [],
      "derived": [],
      "locals": [],
      "callers": [],
      "callees": []
    }],
  "vars": []
}
*/
