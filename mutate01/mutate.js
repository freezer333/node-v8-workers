const addon = require('./build/Release/mutate');

var obj = { 	x: 0  };

// save the target JS object in the addon
addon.setup(obj);
console.log(obj);

addon.mutate(obj);
console.log(obj);

addon.mutate(obj);
console.log(obj);
