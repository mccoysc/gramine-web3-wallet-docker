#!/usr/bin/env node

/**
 * Simple Web3 Application Example
 * This demonstrates how to use Web3.js in a Gramine-protected environment
 */

const Web3 = require('web3');

async function main() {
    console.log('===========================================');
    console.log('Simple Web3 Application Example');
    console.log('Running in Gramine-protected environment');
    console.log('===========================================\n');

    const rpcUrl = process.env.ETH_RPC_URL || 'http://ganache:8545';
    const web3 = new Web3(rpcUrl);

    try {
        const isConnected = await web3.eth.net.isListening();
        console.log(`✓ Connected to Ethereum node: ${isConnected}`);

        const networkId = await web3.eth.net.getId();
        console.log(`✓ Network ID: ${networkId}`);

        const blockNumber = await web3.eth.getBlockNumber();
        console.log(`✓ Latest block number: ${blockNumber}`);

        const accounts = await web3.eth.getAccounts();
        console.log(`✓ Available accounts: ${accounts.length}`);

        if (accounts.length > 0) {
            console.log('\nAccount details:');
            for (let i = 0; i < Math.min(accounts.length, 3); i++) {
                const balance = await web3.eth.getBalance(accounts[i]);
                const balanceEth = web3.utils.fromWei(balance, 'ether');
                console.log(`  Account ${i}: ${accounts[i]}`);
                console.log(`  Balance: ${balanceEth} ETH`);
            }
        }

        console.log('\n✓ Web3 application running successfully in Gramine!');
    } catch (error) {
        console.error('✗ Error:', error.message);
        process.exit(1);
    }
}

main().catch(console.error);
