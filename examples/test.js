var timers = require('timers');

log("running test...");

// Oh, how I wish I had a testsuite running already...

// Run a lot of timeouts at the same time
//for(var i = 0; i < 100; i++){
//  timers.setTimeout(function(now){
//    log("Timed out in " + (Date.now() - now) + "ms");
//  }, 2000, Date.now());
//}
//
//function doTimeout(){
//  timers.setTimeout(function(now){
//    log("Timed out in " + (Date.now() - now) + "ms");
//    doTimeout();
//  }, 100, Date.now());
//}
//doTimeout();
//
// Test clearing a timer
//var timer1 = timers.setTimeout(function(){
//  log('Error: We should not get here, as the timeout was canceled');
//}, 900);
//
//timers.setTimeout(function(){
//  log('Fine, this one wasn not cleared');
//}, 1100);
//
//timers.setTimeout(function(){
//  log('Clearing timer...');
//  timers.clearTimeout(timer1);
//}, 500);

// Test interval
var timer = timers.setInterval(function(now){
  log("Time passed: " + (Date.now() - now) + "ms");
}, 100, Date.now());
timers.setTimeout(function(){
  log('Clearing interval...');
  timers.clearInterval(timer);
}, 500);