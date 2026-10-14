# Spreadshit

A spreadsheet processor that is shit;

- I don't support many features;
- It isn't efficient;
- It has tight memory bounds to avoind doing silly little heap allocations
- It is very simple to read and reason about

# Compile

If you have a working C compiler and make installed you can just run:
```sh
$ make
```

If you don't have this things working, please go get them right now!

# Usage:

```sh
$ printf 'name,age\ngustavo,10\n' | ./main
# name,age
# gustavo,10
```

We support simple formulas:
```sh
$ printf 'name,min_age,max_age,avg\ngustavo,10,18,=(B1+C1)/2' | ./main
# name,min_age,max_age,avg
# gustavo,10,18,14
```

To see more usages just check the `./tests` folder.


