const addon = require('./build/Release/mutate');

var obj = { 	x: 0  };

addon.start(obj);

setInterval( () => {
	addon.let_worker_work();
	console.log(obj)
}, 1000);
