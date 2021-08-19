const hello = require("./index.js");
const original = require("fibers");
const pthread = require("../node-fibers");
const { performance } = require('perf_hooks');
//const Future = require("fibers/future");

function runAnAsync() {
  /*new Fiber(() => {
    const future = new Future();
    this.setTimeout(() => {
      future.return();
    }, 100);
    future.wait();
    console.log("here");
  }).run();*/
  let context = "test";
  console.log("pre-fork", this, new Date());
  hello.fork(() => {
    console.log("hello", this, new Date());
    setTimeout(() => {
      context = "tester";
      hello.resume();
    }, 1000);
    hello.wait();
    console.log("there", this, new Date());
    setTimeout(() => {
      context = "tester2";
      hello.resume();
    }, 1000);
    hello.wait();
    console.log("there1", this, new Date());
  });
  console.log("return", this, new Date());
  //hello.join();
  //console.log("join");
}

function runInFiber() {
  const fiber = new Fiber((test) => {
    console.log("pre-yield", test);
    Fiber.yield();
    console.log("post yield1");
    Fiber.yield();
    console.log("post yield2");
  });

  fiber.run("test");

  setInterval(() => {
    fiber.run();
  }, 1000);
}

function runInFakeFiber() {
  const fiber = new hello.Fiber((test) => {
    console.log("--hello", test);
    test = hello.Fiber.yield();
    console.log("--again", test);
  });

  fiber.run("test");
  console.log("--here");

  setInterval(() => {
    console.log("--resuming");
    fiber.run(Math.random());
  }, Math.random() * 1000);
}

function run(aFiber) {
  const fiber2 = new aFiber((test) => {
    console.log(2, "--hello", test);
    test = aFiber.yield();
    console.log(2, "--again", test);
  });
  const fiber1 = new aFiber((test) => {
    console.log(1, "--hello", test);
    test = aFiber.yield();
    fiber2.run();
    console.log(1, "--again", test);
  });

  fiber1.run("test");

  setInterval(() => {
    console.log("--resuming");
    fiber1.run(Math.random());
  }, 100);
}

function simpleYieldLoop(aFiber) {
  let fiber = new aFiber(() => {
    for (let i = 0; i < 1000; i++) {
      aFiber.yield();
    }
  });
  for (let i = 0; i < 1000; i++) {
    fiber.run();
  }
}

function newThreadYield(aFiber) {
  let count = 0;
  let fiber = new aFiber((count) => {
    for (let i = 0; i < 100; i++) {
      count = aFiber.yield(count + 1);
    }
    return count;
  });
  for (let i = 0; i < 1000; i++) {
    count = fiber.run(count + 1);
  }
}

function newFiber(aFiber) {
  for (let i = 0; i < 100; i++) {
    let fiber = new aFiber(() => {
      throw new Error("");
    });
    try {
      const result = fiber.run();
    }
    catch (e) {
    }
  }
}

let count = 0;
function currentFiber(aFiber) {
  for (let i = 0; i < 100; i++) {
    let fiber = new aFiber(() => {
      aFiber.yield();
    });
    const result = fiber.run();
  }
}

function fiberInFiber(aFiber) {
  try {
  	aFiber(function() {
  		var that = Fiber.current;
  		aFiber(function(){
  			that.run();
  		}).run();
  	}).run();
  } catch(err) {
  	console.log('pass');
  }
}
const names = new Map([[hello.Fiber, "new"], [original, "original"], [pthread, "pthread"]]);
function measureerformance(fn) {
  let lastFiber;
  const results = new Map([["new", []], ["original", []], ["pthread", []]]);
  for (let i = 0; i < 300; i++) {
    const rand = Math.random();
    let aFiber;
    if (rand < 0.333) {
      aFiber = hello.Fiber;
    }
    else if (rand < 0.667) {
      aFiber = pthread;
    }
    else {
      aFiber = original;
    }
    lastFiber = aFiber;
    const start = performance.now();
    fn(aFiber);

    const end = performance.now();
    const name = names.get(aFiber);
    results.get(name).push(end - start);
  }
  console.log("new", results.get("new").reduce((a, b) => a + b, 0) / results.get("new").length);
  console.log("original", results.get("original").reduce((a, b) => a + b, 0) / results.get("original").length);
  console.log("pthread", results.get("pthread").reduce((a, b) => a + b, 0) / results.get("pthread").length);
}
//currentFiber(Fiber);
measureerformance(simpleYieldLoop);
