import fs from "fs";

export class Fiber {
  constructor(fn) {

  }

  static yield() {

  }
}

export class Future {
  #error;
  #value;
  #fd;
  #size;
  wait() {
    let size;
    debugger;
    do {
      size = fs.readFileSync(this.#fd, 'utf8').length;
    } while(size == this.#size);

    if (this.#error) {
      throw this.#error;
    }
    return this.#value;
  }

  constructor() {
    fs.writeFileSync(`/tmp/test`, "");
    this.#fd = fs.openSync(`/tmp/test`);
    this.#size = fs.fstatSync(this.#fd).size;
  }

  throw(err) {
    fs.writeSync(this.#fd, "0");
    this.#error = err;
  }

  return(value) {
    fs.writeSync(this.#fd, "0");
    this.#value = value;
  }
}
