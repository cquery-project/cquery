union vector3 {
  struct { float x, y, z; };
  float v[3];
};

/*
OUTPUT:
{
  "types": [{
      "id": 0,
      "usr": "c:@U@vector3",
      "short_name": "vector3",
      "qualified_name": "vector3",
      "definition_spelling": "1:7-1:14",
      "definition_extent": "1:1-4:2",
      "vars": [3],
      "uses": ["*1:7-1:14"]
    }, {
      "id": 1,
      "usr": "c:@U@vector3@Sa",
      "short_name": "<anonymous>",
      "qualified_name": "vector3::<anonymous>",
      "definition_spelling": "2:3-2:9",
      "definition_extent": "2:3-2:28",
      "vars": [0, 1, 2],
      "uses": ["*2:3-2:9"]
    }],
  "vars": [{
      "id": 0,
      "usr": "c:@U@vector3@Sa@FI@x",
      "short_name": "x",
      "qualified_name": "x",
      "definition_spelling": "2:18-2:19",
      "definition_extent": "2:12-2:19",
      "declaring_type": 1,
      "uses": ["2:18-2:19"]
    }, {
      "id": 1,
      "usr": "c:@U@vector3@Sa@FI@y",
      "short_name": "y",
      "qualified_name": "y",
      "definition_spelling": "2:21-2:22",
      "definition_extent": "2:12-2:22",
      "declaring_type": 1,
      "uses": ["2:21-2:22"]
    }, {
      "id": 2,
      "usr": "c:@U@vector3@Sa@FI@z",
      "short_name": "z",
      "qualified_name": "z",
      "definition_spelling": "2:24-2:25",
      "definition_extent": "2:12-2:25",
      "declaring_type": 1,
      "uses": ["2:24-2:25"]
    }, {
      "id": 3,
      "usr": "c:@U@vector3@FI@v",
      "short_name": "v",
      "qualified_name": "vector3::v",
      "definition_spelling": "3:9-3:10",
      "definition_extent": "3:3-3:13",
      "declaring_type": 0,
      "uses": ["3:9-3:10"]
    }]
}
*/
