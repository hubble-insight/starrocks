---
displayed_sidebar: docs
---

# split_part

## Description

This function splits a given string according to the separator and returns the requested part. `field` starts counting from 1: a positive `field` counts parts from the beginning of the string, and a negative `field` counts parts from the end.

## Syntax

```Haskell
VARCHAR split_part(VARCHAR content, VARCHAR delimiter, INT field)
```

## Parameters

`content`: the string to be split. VARCHAR is supported.

`delimiter`: the separator. VARCHAR is supported.

`field`: the index of the part to return. INT is supported.

## Return values

A VARCHAR value is returned.

- If `field` is 0, an empty string is returned.
- If the delimiter does not exist in the string:
  - If `field` is 1 or -1, the original string is returned (the original string is the only part).
  - Otherwise, an empty string is returned.
- If `field` exceeds the number of parts (a positive `field` is greater than the number of parts, or the absolute value of a negative `field` exceeds the number of parts), an empty string is returned.
- If the delimiter is an empty string, the string is split into UTF-8 characters. `field` indexes characters from the beginning (positive) or from the end (negative). An empty string is returned if `field` is out of range.
- If any argument is NULL, NULL is returned.

## Examples

```Plain Text
MySQL > select split_part("hello world", " ", 1);
+----------------------------------+
|split_part('hello world', ' ', 1) |
+----------------------------------+
| hello                            |
+----------------------------------+

MySQL > select split_part("hello world", " ", 2);
+-----------------------------------+
| split_part('hello world', ' ', 2) |
+-----------------------------------+
| world                             |
+-----------------------------------+

MySQL > select split_part("hello world", " ", -1);
+----------------------------------+
|split_part('hello world', ' ', -1) |
+----------------------------------+
| world                            |
+----------------------------------+

MySQL > select split_part("hello world", " ", -2);
+-----------------------------------+
| split_part('hello world', ' ', -2) |
+-----------------------------------+
| hello                             |
+-----------------------------------+

MySQL > select split_part("abca", "a", 1);
+----------------------------+
| split_part('abca', 'a', 1) |
+----------------------------+
|                            |
+----------------------------+

select split_part("abca", "a", -1);
+-----------------------------+
| split_part('abca', 'a', -1) |
+-----------------------------+
|                             |
+-----------------------------+

select split_part("abca", "a", -2);
+-----------------------------+
| split_part('abca', 'a', -2) |
+-----------------------------+
| bc                          |
+-----------------------------+

MySQL > select split_part("hello world", ",", 1);
+-----------------------------------+
| split_part('hello world', ',', 1) |
+-----------------------------------+
| hello world                       |
+-----------------------------------+

MySQL > select split_part("hello world", " ", 3);
+-----------------------------------+
| split_part('hello world', ' ', 3) |
+-----------------------------------+
|                                   |
+-----------------------------------+

MySQL > select split_part("hello world", " ", 0);
+-----------------------------------+
| split_part('hello world', ' ', 0) |
+-----------------------------------+
|                                   |
+-----------------------------------+

MySQL > select split_part("abc", "", 2);
+---------------------------+
| split_part('abc', '', 2) |
+---------------------------+
| b                         |
+---------------------------+
```

## keyword

SPLIT_PART,SPLIT,PART
