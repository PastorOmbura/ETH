pragma solidity ^0.5.8;

contract Salemcash {

    mapping(address => uint) private _balances;

    constructor() public {
        _balances[msg.sender] = 20000000000000000000000000;
    }

    function getBalance(address account) public view returns (uint) {
        return _balances[account];
    }

    function transfer(address to, uint amount) public {
        require(_balances[msg.sender] >= amount);

        _balances[msg.sender] -= amount;
        _balances[to] += amount;
    }
}
