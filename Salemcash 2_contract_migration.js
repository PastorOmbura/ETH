
var SampleToken = artifacts.require("./Salemcash.sol");
const _name = "Salemcash";
const _symbol = "SCS";
const _decimals = 18;
const _total_supply = 20000000000000000000000000;
module.exports = function(deployer) {
deployer.deploy(SampleToken, _name, _symbol, _decimals, _total_supply);
};