struct ForwardType;
void foo(ForwardType*) {}
/*
OUTPUT:
{
  "includes": [],
  "skipped_by_preprocessor": [],
  "types": [{
      "id": 0,
      "usr": "c:@S@ForwardType",
      "short_name": "",
      "detailed_name": "",
      "definition_spelling": "-1:-1--1:-1",
      "definition_extent": "-1:-1--1:-1",
      "parents": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [],
      "uses": ["1:8-1:19", "2:10-2:21"]
    }],
  "funcs": [{
      "id": 0,
      "is_operator": false,
      "usr": "c:@F@foo#*$@S@ForwardType#",
      "short_name": "foo",
      "detailed_name": "void foo(ForwardType *)",
      "declarations": [],
      "definition_spelling": "2:6-2:9",
      "definition_extent": "2:1-2:26",
      "base": [],
      "derived": [],
      "locals": [],
      "callers": [],
      "callees": []
    }],
  "vars": []
}
*/
