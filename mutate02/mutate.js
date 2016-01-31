const addon = require('./build/Release/mutate');

var obj = { 	x: 0  };

addon.start(obj);

setInterval( () => {
	console.log(obj)
}, 1000);
