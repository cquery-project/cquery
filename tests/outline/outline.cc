#include <vector>

struct MergeableUpdate {
  int a;
  int b;
  std::vector<int> to_add;
};

/*
TEXT_REPLACE:
c:@N@std@ST>2#T#T@vector <===> c:@N@std@N@__1@ST>2#T#T@vector

OUTPUT:
{
  "includes": [{
      "line": 1,
      "resolved_path": "&vector"
    }],
  "skipped_by_preprocessor": [],
  "types": [{
      "id": 0,
      "usr": "c:@S@MergeableUpdate",
      "short_name": "MergeableUpdate",
      "detailed_name": "MergeableUpdate",
      "definition_spelling": "3:8-3:23",
      "definition_extent": "3:1-7:2",
      "parents": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [0, 1, 2],
      "instances": [],
      "uses": ["3:8-3:23"]
    }, {
      "id": 1,
      "usr": "c:@N@std@N@__1@ST>2#T#T@vector",
      "short_name": "",
      "detailed_name": "",
      "parents": [],
      "derived": [],
      "types": [],
      "funcs": [],
      "vars": [],
      "instances": [2],
      "uses": ["6:8-6:14"]
    }],
  "funcs": [],
  "vars": [{
      "id": 0,
      "usr": "c:@S@MergeableUpdate@FI@a",
      "short_name": "a",
      "detailed_name": "int MergeableUpdate::a",
      "definition_spelling": "4:7-4:8",
      "definition_extent": "4:3-4:8",
      "declaring_type": 0,
      "cls": 4,
      "uses": ["4:7-4:8"]
    }, {
      "id": 1,
      "usr": "c:@S@MergeableUpdate@FI@b",
      "short_name": "b",
      "detailed_name": "int MergeableUpdate::b",
      "definition_spelling": "5:7-5:8",
      "definition_extent": "5:3-5:8",
      "declaring_type": 0,
      "cls": 4,
      "uses": ["5:7-5:8"]
    }, {
      "id": 2,
      "usr": "c:@S@MergeableUpdate@FI@to_add",
      "short_name": "to_add",
      "detailed_name": "std::vector<int> MergeableUpdate::to_add",
      "definition_spelling": "6:20-6:26",
      "definition_extent": "6:3-6:26",
      "variable_type": 1,
      "declaring_type": 0,
      "cls": 4,
      "uses": ["6:20-6:26"]
    }]
}
*/
